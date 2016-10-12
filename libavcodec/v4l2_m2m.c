/*
 * V4L mem2mem wrapper
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

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dirent.h>


#include "libavutil/imgutils.h"
#include "libavutil/pixfmt.h"
#include "libavutil/pixdesc.h"
#include "avcodec.h"
#include "v4l2-buffers.h"
#include "v4l2-common.h"
#include "v4l2_m2m.h"
#include "v4l2_m2m_avcodec.h"
#include "v4l2.h"

#define V4L_MAX_STREAM_SIZE (2*1024*1024)

static const char opool_name[] =  "output pool";
static const char cpool_name[] = "capture pool";

static inline int try_raw_format(V4LBufferPool* bp, enum AVPixelFormat pixfmt) {
    struct v4l2_format *fmt = &bp->format;
    int i;

    fmt->type  = bp->type;

    if(V4L2_TYPE_IS_MULTIPLANAR(bp->type)) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pixfmt);

        fmt->fmt.pix_mp.width  = bp->width;
        fmt->fmt.pix_mp.height = bp->height;

        fmt->fmt.pix_mp.num_planes = av_pix_fmt_count_planes(pixfmt);

        for(i=0; i<fmt->fmt.pix_mp.num_planes; i++) {
            uint32_t h;
            fmt->fmt.pix_mp.plane_fmt[i].bytesperline = av_image_get_linesize(pixfmt, bp->width, i);

            if (i == 1 || i == 2) {
                h = FF_CEIL_RSHIFT(bp->height, desc->log2_chroma_h);
            } else
                h = bp->height;

            fmt->fmt.pix_mp.plane_fmt[i].sizeimage = fmt->fmt.pix_mp.plane_fmt[i].bytesperline * h;
        }

        fmt->fmt.pix_mp.pixelformat = avpriv_v4l_fmt_ff2v4l(pixfmt, bp->av_codec_id, FF_V4L_PACK_AVFRAME);
        if(fmt->fmt.pix_mp.pixelformat && !ioctl(bp->fd, VIDIOC_TRY_FMT, fmt))
            return 0;
    } else {
        fmt->fmt.pix.width  = bp->width;
        fmt->fmt.pix.height = bp->height;

        fmt->fmt.pix.bytesperline = av_image_get_linesize(pixfmt, bp->width, 0);
        fmt->fmt.pix.sizeimage    = fmt->fmt.pix.bytesperline * bp->height;
        fmt->fmt.pix.pixelformat  = avpriv_v4l_fmt_ff2v4l(pixfmt, bp->av_codec_id, FF_V4L_PACK_AVFRAME);

        if(fmt->fmt.pix.pixelformat && !ioctl(bp->fd, VIDIOC_TRY_FMT, fmt))
            return 0;
    }

    return AVERROR(EINVAL);
}

static int set_raw_format(V4LBufferPool* bp, int set) {
    struct v4l2_format *fmt = &bp->format;
    enum AVPixelFormat pixfmt = bp->av_pix_fmt;
    struct v4l2_fmtdesc fmtdesc = { 0 };

    fmtdesc.type = bp->type;

    if(AV_PIX_FMT_NONE != pixfmt) {
        if(try_raw_format(bp, pixfmt)) {
            if(set)
                av_log(bp->log_ctx, AV_LOG_INFO, "Suggested pixel format %s is not accepted on %s, will guess one.\n", av_get_pix_fmt_name(pixfmt), bp->name);
            pixfmt = AV_PIX_FMT_NONE;
        }
    }

    while(AV_PIX_FMT_NONE == pixfmt && !ioctl(bp->fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
        pixfmt = avpriv_v4l_fmt_v4l2ff(fmtdesc.pixelformat, AV_CODEC_ID_RAWVIDEO);
        if(try_raw_format(bp, pixfmt))
            pixfmt = AV_PIX_FMT_NONE;
        if(AV_PIX_FMT_NONE != pixfmt && set) {
            av_log(bp->log_ctx, AV_LOG_INFO, "Pixelformat %s is accepted on %s, using it.\n", av_get_pix_fmt_name(pixfmt), bp->name);
            bp->av_pix_fmt = pixfmt;
        }
        fmtdesc.index++;
    }

    if(AV_PIX_FMT_NONE == pixfmt)
        return AVERROR(EINVAL);

    return (set ? ioctl(bp->fd, VIDIOC_S_FMT, fmt) : 0);
}

static int set_coded_format(V4LBufferPool* bp, int set) {
    struct v4l2_format *fmt = &bp->format;

    fmt->type = bp->type;

    if(V4L2_TYPE_IS_MULTIPLANAR(bp->type)) {
        fmt->fmt.pix_mp.num_planes = VIDEO_MAX_PLANES;
        fmt->fmt.pix_mp.pixelformat = avpriv_v4l_fmt_ff2v4l(bp->av_pix_fmt, bp->av_codec_id, FF_V4L_PACK_AVPACKET);
        if(!fmt->fmt.pix_mp.pixelformat) {
            av_log(bp->log_ctx, AV_LOG_ERROR, "No matching V4L Codec ID found for %i\n", bp->av_codec_id);
            return AVERROR(EINVAL);
        }
        fmt->fmt.pix_mp.plane_fmt[0].sizeimage = V4L_MAX_STREAM_SIZE;
    } else {
        fmt->fmt.pix.pixelformat = avpriv_v4l_fmt_ff2v4l(bp->av_pix_fmt, bp->av_codec_id, FF_V4L_PACK_AVPACKET);
        if(!fmt->fmt.pix.pixelformat) {
            av_log(bp->log_ctx, AV_LOG_ERROR, "No matching V4L Codec ID found for %i\n", bp->av_codec_id);
            return AVERROR(EINVAL);
        }
        fmt->fmt.pix.sizeimage = V4L_MAX_STREAM_SIZE;
    }
    return (set ? ioctl(bp->fd, VIDIOC_S_FMT, fmt) : ioctl(bp->fd, VIDIOC_TRY_FMT, fmt));
}

int avpriv_set_pool_format(V4LBufferPool* bp, int set) {
    if(AV_CODEC_ID_RAWVIDEO == bp->av_codec_id)
        return set_raw_format(bp, set);
   return set_coded_format(bp, set);
}

static int probe_and_set(V4Lm2mContext* s, void *log_ctx, int set) {
    int ret;

    int fail_log_level = ( set ? AV_LOG_ERROR : AV_LOG_DEBUG);

    s->fd = open(s->devname, O_RDWR | O_NONBLOCK, 0);

    if(s->fd < 0)
        return AVERROR(errno);

    s->capture_pool.fd      = s->fd;
    s->capture_pool.log_ctx = log_ctx;
    s->capture_pool.name    = cpool_name;
    s->output_pool.fd       = s->fd;
    s->output_pool.log_ctx  = log_ctx;
    s->output_pool.name     = opool_name;

    if((ret = ioctl(s->fd, VIDIOC_QUERYCAP, &(s->cap))) < 0)
        goto fail;

#define CHECK_CAPS(s, ncap) ((s->cap.capabilities & (ncap)) == (ncap))
    if(CHECK_CAPS(s, V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING) || CHECK_CAPS(s, V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE | V4L2_CAP_STREAMING)) {
        s->capture_pool.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        s->output_pool.type  = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    } else if(CHECK_CAPS(s, V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING) || CHECK_CAPS(s, V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING)) {
        s->capture_pool.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        s->output_pool.type  = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    } else {
        av_log(log_ctx, fail_log_level, "Sorry, driver '%s' on card '%s' is not a V4L mem2mem device\n", s->cap.driver, s->cap.card);
        ret = AVERROR(EINVAL);
        goto fail;
    }
#undef CHECK_CAPS

    if(s->output_pool_needs_format && (ret = avpriv_set_pool_format(&s->output_pool, set))) {
        av_log(log_ctx, fail_log_level, "Failed to set input format\n");
        goto fail;
    }

    if(s->capture_pool_needs_format && (ret = avpriv_set_pool_format(&s->capture_pool, set))) {
        av_log(log_ctx, fail_log_level, "Failed to set output format\n");
        goto fail;
    }

    if(s->output_pool_needs_init && set && (ret = avpriv_init_v4lbufpool(&(s->output_pool)))) {
        av_log(log_ctx, fail_log_level, "Failed to request output pool's buffers\n");
        goto fail;
    }

    if(s->capture_pool_needs_init && set && (ret = avpriv_init_v4lbufpool(&(s->capture_pool)))) {
        av_log(log_ctx, fail_log_level, "Failed to request capture pool's buffers\n");
        goto fail;
    }

fail:
    if(!set || ret) {
        close(s->fd);
        s->fd = 0;
    }
    return ret;
}

int avpriv_v4lm2m_init(V4Lm2mContext* s, void* log_ctx) {
    int ret = AVERROR(EINVAL);

    if(s->devname && *s->devname) {
        return probe_and_set(s, log_ctx, 1);
    } else {
        DIR *dirp;
        struct dirent *dp;
        const char pref[] = "video";
        char *devname_save = s->devname;
        char tmpbuf[PATH_MAX];

        av_log(log_ctx, AV_LOG_INFO, "Device path not set, probing /dev/video*\n");

        if(!(dirp = opendir("/dev")))
            return AVERROR(errno);

        for(dp = readdir(dirp); dp; dp = readdir(dirp)) {
            if(!strncmp(dp->d_name, pref, sizeof(pref)-1)) {
                errno = 0;
                snprintf(tmpbuf, sizeof(tmpbuf)-1, "/dev/%s", dp->d_name);
                av_log(log_ctx, AV_LOG_DEBUG, "Probing %s\n", tmpbuf);
                s->devname = tmpbuf;
                if(!(ret = probe_and_set(s, log_ctx, 0)))
                    break;
            }
        }

        closedir(dirp);

        if(!ret) {
            av_log(log_ctx, AV_LOG_INFO, "Using device %s\n", tmpbuf);
            ret = probe_and_set(s, log_ctx, 1);
        } else {
            av_log(log_ctx, AV_LOG_ERROR, "Could not find a valid device\n");
        }

        s->devname = devname_save;
    }

    return ret;
}


int ff_v4lm2m_codec_init(AVCodecContext *avctx) {
    V4Lm2mContext *s = avctx->priv_data;
    return avpriv_v4lm2m_init(s, avctx);
}

int avpriv_v4lm2m_end(V4Lm2mContext* s) {
    avpriv_release_buffer_pool(&s->output_pool);
    avpriv_release_buffer_pool(&s->capture_pool);

    avpriv_set_stream_status(&s->output_pool, VIDIOC_STREAMOFF);
    avpriv_set_stream_status(&s->capture_pool, VIDIOC_STREAMOFF);

    close(s->fd);
    return 0;
}

int ff_v4lm2m_codec_end(AVCodecContext *avctx) {
    V4Lm2mContext *s = avctx->priv_data;

    av_log(avctx, AV_LOG_DEBUG, "Closing context\n");
    return avpriv_v4lm2m_end(s);
}
