/*
 * V4L2 buffer{,pool} helper functions.
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

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include "avcodec.h"
#include "internal.h"
#include "v4l2.h"
#include "v4l2-buffers.h"
#include "v4l2-common.h"

#define IS_BP_SUPPORTED(bp) ((bp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) || (bp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) || (bp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) || (bp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT))

// Define it if you want more AV_LOG_DEBUG output
#define V4L_BUFFER_DEBUG

/*
 * Missing features:
 *  - USERPTR support is a bit rough
 *  - DMABUF ?
 */

enum V4LBuffer_status {
    V4LBUF_AVAILABLE,
    V4LBUF_IN_DRIVER,
    V4LBUF_RET_USER,
};

struct V4LBuffer {
    struct V4LBufferPool *pool;
    int index;
    int num_planes;
    int linesize[4];
    size_t lengths[VIDEO_MAX_PLANES];
    void* mmap_addr[VIDEO_MAX_PLANES];
    AVBufferRef *bufrefs[VIDEO_MAX_PLANES];
    enum V4LBuffer_status status;
    int flags;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buf;
    struct timeval timestamp;
    int ref_cnt;
    V4LBuffer *piped;
};

static void buffer_callback(void *opaque, uint8_t *unused);

static int enqueue_v4lbuf(V4LBuffer* avbuf) {
    int ret;
    memset(&avbuf->buf, 0, sizeof(avbuf->buf));

    avbuf->buf.type = avbuf->pool->type;
    avbuf->buf.memory = avbuf->pool->memory;
    avbuf->buf.index = avbuf->index;

    if(V4L2_TYPE_IS_MULTIPLANAR(avbuf->pool->type)) {
        avbuf->buf.m.planes = avbuf->planes;
        avbuf->buf.length   = avbuf->num_planes;
    } else {
        avbuf->buf.length    = avbuf->planes[avbuf->index].length;
        avbuf->buf.bytesused = avbuf->planes[avbuf->index].bytesused;
        avbuf->buf.m.userptr = avbuf->planes[avbuf->index].m.userptr;
    }

    avbuf->buf.timestamp = avbuf->timestamp;
    avbuf->buf.flags     = avbuf->pool->default_flags | avbuf->flags;

    if((ret = avbuf->pool->ioctl_f(avbuf->pool->fd, VIDIOC_QBUF, &avbuf->buf)) < 0)
        return AVERROR(errno);
    avbuf->status = V4LBUF_IN_DRIVER;
    avbuf->pool->num_queued++;

#ifdef V4L_BUFFER_DEBUG
    av_log(avbuf->pool->log_ctx, AV_LOG_DEBUG, "Successfuly enqueued a buffer on %s\n", avbuf->pool->name);
#endif

    return 0;
}

static V4LBuffer* dequeue_v4lbuf(V4LBufferPool *bp) {
    int ret;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buf = { 0 };
    V4LBuffer* avbuf = NULL;
    int i;

    if(bp->num_queued <= bp->min_queued_buffers) {
        if(!V4L2_TYPE_IS_OUTPUT(bp->type))
            av_log(bp->log_ctx, AV_LOG_WARNING, "Trying to dequeue on %s with %i buffers queued and requiring %i buffers\n", bp->name, bp->num_queued, bp->min_queued_buffers);
        return NULL;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = bp->type;
    buf.memory = bp->memory;

    if(V4L2_TYPE_IS_MULTIPLANAR(bp->type)) {
        memset(planes, 0, sizeof(planes));
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
    }

    if(bp->blocking_dequeue) {
        struct pollfd pfd = { .fd = bp->fd, .events = POLLIN | POLLERR };
        av_log(bp->log_ctx, AV_LOG_DEBUG, "Waiting for event for %i msec before dequeuing on %s\n", bp->blocking_dequeue, bp->name);
        if((ret = poll(&pfd, 1, bp->blocking_dequeue)) <= 0) {
            av_log(bp->log_ctx, AV_LOG_WARNING, "No event occurred while waiting\n");
            return NULL;
        }
    }

    ret = bp->ioctl_f(bp->fd, VIDIOC_DQBUF, &buf);
    if(ret)
        return NULL;

    avbuf = &(bp->buffers[buf.index]);
    avbuf->status = V4LBUF_AVAILABLE;
    avbuf->buf = buf;

    if(V4L2_TYPE_IS_MULTIPLANAR(bp->type)) {
        memcpy(avbuf->planes, planes, sizeof(planes));
        avbuf->buf.m.planes = avbuf->planes;
    }
    avbuf->pool->num_queued--;

    /*
     * unref buffers if any for output pools.
     * capture pools will send it back to user and thus we need to keep the ref for now.
     */
    if(V4L2_TYPE_IS_OUTPUT(avbuf->pool->type)) {
        for(i=0; i<avbuf->num_planes; i++) {
            if(avbuf->bufrefs[i]) {
                av_buffer_unref(&avbuf->bufrefs[i]);
            }
        }
    }

#ifdef V4L_BUFFER_DEBUG
    av_log(bp->log_ctx, AV_LOG_DEBUG, "Successfuly dequeued a buffer on %s\n", bp->name);
#endif

    return avbuf;
}

static inline int init_buffer(V4LBuffer* avbuf) {
    int ret, i;

    avbuf->buf.type = avbuf->pool->type;
    avbuf->buf.memory = avbuf->pool->memory;
    avbuf->buf.index = avbuf->index;
    if(V4L2_TYPE_IS_MULTIPLANAR(avbuf->pool->type)) {
        avbuf->buf.length = VIDEO_MAX_PLANES;
        avbuf->buf.m.planes = avbuf->planes;
    }

    if((ret= avbuf->pool->ioctl_f(avbuf->pool->fd, VIDIOC_QUERYBUF, &avbuf->buf)) < 0)
        return AVERROR(errno);

    if(V4L2_TYPE_IS_MULTIPLANAR(avbuf->pool->type)) {
        for(avbuf->num_planes=0; avbuf->num_planes < avbuf->buf.length && avbuf->buf.m.planes[avbuf->num_planes].length; avbuf->num_planes++);
    } else avbuf->num_planes = 1;

    for(i=0; i < avbuf->num_planes; i++) {
        if(V4L2_TYPE_IS_MULTIPLANAR(avbuf->pool->type)) {
            avbuf->linesize[i] = avbuf->pool->format.fmt.pix_mp.plane_fmt[i].bytesperline;
        } else
            avbuf->linesize[i] = avbuf->pool->format.fmt.pix.bytesperline;

        switch(avbuf->pool->memory) {
            case V4L2_MEMORY_MMAP:
                if(V4L2_TYPE_IS_MULTIPLANAR(avbuf->pool->type)) {
                    avbuf->mmap_addr[i] = avbuf->pool->mmap_f(NULL, avbuf->buf.m.planes[i].length,
                            PROT_READ | PROT_WRITE, MAP_SHARED,
                            avbuf->pool->fd, avbuf->buf.m.planes[i].m.mem_offset);
                    avbuf->lengths[i] = avbuf->buf.m.planes[i].length;
                } else {
                    avbuf->mmap_addr[i] = avbuf->pool->mmap_f(NULL, avbuf->buf.length,
                            PROT_READ | PROT_WRITE, MAP_SHARED,
                            avbuf->pool->fd, avbuf->buf.m.offset);
                    avbuf->lengths[i] = avbuf->buf.length;
                }

                if(avbuf->mmap_addr[i] == MAP_FAILED)
                    return AVERROR(ENOMEM);

                break;
            case V4L2_MEMORY_USERPTR:
                /* Nothing to do */
                break;
            default:
                av_log(avbuf->pool->log_ctx, AV_LOG_ERROR, "%s: Sorry, memory type %i is not yet supported\n", __func__, avbuf->pool->memory);
                return AVERROR_PATCHWELCOME;
        }
    }
    avbuf->status = V4LBUF_AVAILABLE;

    av_log(avbuf->pool->log_ctx, AV_LOG_DEBUG, "Successfully created %i plane(s) for buffer %i in %s\n", avbuf->num_planes, avbuf->buf.index, avbuf->pool->name);

    /* feed capture buffers; can't enqueue userptr buffers now */
    if(!V4L2_TYPE_IS_OUTPUT(avbuf->pool->type) && (avbuf->pool->memory != V4L2_MEMORY_USERPTR))
        return enqueue_v4lbuf(avbuf);

    return 0;
}

int avpriv_init_v4lbufpool(V4LBufferPool* bufs) {
    struct v4l2_requestbuffers req;
    int ret, i;

    if(!IS_BP_SUPPORTED(bufs)) {
        av_log(bufs->log_ctx, AV_LOG_ERROR, "%s: Sorry, type %i is not supported\n", __func__, bufs->type);
        return AVERROR_PATCHWELCOME;
    }

#define SET_WRAPPER(F) if(!bufs->F ## _f) bufs->F ## _f = F
    SET_WRAPPER(open);
    SET_WRAPPER(close);
    SET_WRAPPER(dup);
    SET_WRAPPER(ioctl);
    SET_WRAPPER(read);
    SET_WRAPPER(mmap);
    SET_WRAPPER(munmap);
#undef SET_WRAPPER

    memset(&req, 0, sizeof(req));
    req.count = bufs->num_buffers + bufs->min_queued_buffers;
    req.type = bufs->type;
    req.memory = bufs->memory;

    if((ret = bufs->ioctl_f(bufs->fd, VIDIOC_REQBUFS, &req)) < 0)
        return AVERROR(errno);

    if(bufs->num_buffers > req.count)
        av_log(bufs->log_ctx, AV_LOG_WARNING, "Requested %i buffers but driver gave us only %i for %s\n", bufs->num_buffers, req.count, bufs->name);

    bufs->num_buffers = req.count;
    bufs->num_queued  = 0;
    bufs->buffers = av_mallocz(bufs->num_buffers * sizeof(V4LBuffer));

    for(i=0; i < req.count; i++) {
        V4LBuffer *avbuf = &bufs->buffers[i];

        avbuf->pool = bufs;
        avbuf->index = i;
        if((ret = init_buffer(avbuf)) < 0) {
            av_log(bufs->log_ctx, AV_LOG_ERROR, "Buffer initialization for %s failed (%s)\n", bufs->name, av_err2str(ret));
            return ret;
        }
    }

    return 0;
}

static void release_buf(V4LBuffer* b) {
    int i;
    for(i=0; i < b->num_planes; i++)
        if(b->mmap_addr[i] && b->lengths[i])
            b->pool->munmap_f(b->mmap_addr[i], b->lengths[i]);
}

void avpriv_release_buffer_pool(V4LBufferPool* bp) {
    if(bp->buffers) {
        int i;
        for(i=0; i < bp->num_buffers; i++)
            release_buf(&bp->buffers[i]);
        av_free(bp->buffers);
    }
}

static int avbuf_to_avbuf(V4LBuffer* ibuf, V4LBuffer* obuf) {
    int i;
    if( ibuf->pool->memory != V4L2_MEMORY_MMAP    ||
        obuf->pool->memory != V4L2_MEMORY_USERPTR ||
        ibuf->num_planes   != obuf->num_planes ) {
        av_log(ibuf->pool->log_ctx, AV_LOG_ERROR, "Buffers do not match:\n");
        av_log(ibuf->pool->log_ctx, AV_LOG_ERROR, "  Memory: %i vs %i.\n", ibuf->pool->memory, obuf->pool->memory);
        av_log(ibuf->pool->log_ctx, AV_LOG_ERROR, "  Planes: %i vs %i.\n", ibuf->num_planes, obuf->num_planes);
        return AVERROR(EINVAL);
    }

    memcpy(obuf->planes , ibuf->planes,  sizeof(obuf->planes ));
    memcpy(obuf->lengths, ibuf->lengths, sizeof(obuf->lengths));

    for(i=0; i<ibuf->num_planes; i++) {
        if(obuf->bufrefs[i])
            av_buffer_unref(&obuf->bufrefs[i]);
        if(ibuf->bufrefs[i])
            av_buffer_unref(&ibuf->bufrefs[i]);

        /*
         * We keep two refs here:
         * - One in obuf, which belongs to a capture pool. The ref will be "returned" to user when dequeueing it.
         *   It will be released when the user frees the frame/packet.
         * - One in ibuf, which belongs to an output pool. The ref will be released when it'll be dequeued from the output pool.
         *   i.e. when the buffer is not used by the driver anymore.
         */
        ibuf->bufrefs[i] = av_buffer_create(ibuf->mmap_addr[i], ibuf->lengths[i], buffer_callback, ibuf, 0);
        if(!ibuf->bufrefs[i]) {
            return AVERROR(ENOMEM);
        }

        obuf->bufrefs[i] = av_buffer_ref(ibuf->bufrefs[i]);
        if(!obuf->bufrefs[i]) {
            return AVERROR(ENOMEM);
        }

        ibuf->ref_cnt++;
        obuf->planes[i].m.userptr = (unsigned long)ibuf->mmap_addr[i];
    }
    ibuf->status = V4LBUF_RET_USER;
    return 0;
}

static inline void set_pts(V4LBuffer *out, int64_t pts) {
    out->timestamp.tv_sec  = pts / INT64_C(1000000);
    out->timestamp.tv_usec = pts % INT64_C(1000000);
}

static inline uint64_t get_pts(V4LBuffer *avbuf) {
    if(avbuf->buf.timestamp.tv_sec || avbuf->buf.timestamp.tv_usec)
        return avbuf->buf.timestamp.tv_sec * INT64_C(1000000) + avbuf->buf.timestamp.tv_usec;
    return AV_NOPTS_VALUE;
}

static int buf2v4l(V4LBuffer *out, int plane, const uint8_t* data, int size, AVBufferRef* bref) {
    if(plane >= out->num_planes) {
        av_log(out->pool->log_ctx, AV_LOG_ERROR, "Trying to feed the %ith plane of a V4L buffer with only %i planes\n", plane+1, out->num_planes);
        return AVERROR(EINVAL);
    }

    switch(out->pool->memory) {
        case V4L2_MEMORY_MMAP:
            if(out->mmap_addr[plane] != data) {
                if(!out->pool->copy_warn) {
                    av_log(out->pool->log_ctx, AV_LOG_WARNING, "Performing useless memcpy() on %s because buffers do not match\n", out->pool->name);
                    av_log(out->pool->log_ctx, AV_LOG_WARNING, "This could be avoided by using av_v4l_buffer_pool_get_buffer*() or av_v4l_buffer_pool_make_pipe()\n");
                    out->pool->copy_warn = 1;
                }
                memcpy(out->mmap_addr[plane], data, FFMIN(size, out->lengths[plane]));
            }
            break;
        case V4L2_MEMORY_USERPTR:
            if(!bref) {
                av_log(out->pool->log_ctx, AV_LOG_ERROR, "%s needs to be fed with an AVBufferRef for USERPTR memory type\n", __func__);
                return AVERROR_PATCHWELCOME;
            }

            if(out->bufrefs[plane]) {
                av_log(out->pool->log_ctx, AV_LOG_WARNING, "%s: V4L buffer already had a buffer referenced\n", __func__);
                av_buffer_unref(&out->bufrefs[plane]);
            }

            out->bufrefs[plane] = av_buffer_ref(bref);

            if(!out->bufrefs[plane])
                return AVERROR(ENOMEM);

            out->planes[plane].m.userptr = (unsigned long)out->bufrefs[plane]->data;
            out->lengths[plane]          = out->bufrefs[plane]->size;
            break;
        default:
            av_log(out->pool->log_ctx, AV_LOG_ERROR, "%s: Sorry, memory type %i not supported", __func__, out->pool->memory);
            return AVERROR_PATCHWELCOME;
    }

    out->planes[plane].bytesused = FFMIN(size, out->lengths[plane]);
    out->planes[plane].length    = out->lengths[plane];

    return 0;
}

static int avframe_to_v4lbuf(const AVFrame *pict, V4LBuffer* out) {
    int i, ret;
    for(i=0; i<out->num_planes; i++) {
        if(ret = buf2v4l(out, i, pict->buf[i]->data, pict->buf[i]->size, pict->buf[i]))
            return ret;
    }
    set_pts(out, pict->pts);
    return 0;
}

static int avpkt_to_v4lbuf(const AVPacket *pkt, V4LBuffer *out) {
    int ret;
    if(ret = buf2v4l(out, 0, pkt->data, pkt->size, pkt->buf))
        return ret;

    if(pkt->pts != AV_NOPTS_VALUE)
        set_pts(out, pkt->pts);

    if(pkt->flags & AV_PKT_FLAG_KEY)
        out->flags = V4L2_BUF_FLAG_KEYFRAME;

    return 0;
}

static inline int v4l2bufref(V4LBuffer *in, int plane, AVBufferRef **buf) {

#ifdef V4L_BUFFER_DEBUG
    av_log(in->pool->log_ctx, AV_LOG_DEBUG, "Making an avbuffer from V4L buffer %i[%i] on %s (%i,%i)\n", in->index, plane, in->pool->name, in->pool->type, in->pool->memory);
#endif

    if(plane >= in->num_planes) {
        av_log(in->pool->log_ctx, AV_LOG_ERROR, "Trying to use the %ith plane of a V4L buffer with only %i planes\n", plane+1, in->num_planes);
        return AVERROR(EINVAL);
    }

    switch(in->pool->memory) {
        case V4L2_MEMORY_MMAP:
            *buf = av_buffer_create(in->mmap_addr[plane], in->lengths[plane], buffer_callback, in, 0);
            if(!*buf)
                return AVERROR(ENOMEM);
            in->ref_cnt++;
            in->status = V4LBUF_RET_USER;
            break;
        case V4L2_MEMORY_USERPTR:
            if(!in->bufrefs[plane]) {
                av_log(in->pool->log_ctx, AV_LOG_ERROR, "Calling %s with a USERPTR buffer but no AVBufferRef has been found...\n", __func__);
                return AVERROR(EINVAL);
            }
            *buf = av_buffer_ref(in->bufrefs[plane]);
            if(!*buf)
                return AVERROR(ENOMEM);
            av_buffer_unref(&in->bufrefs[plane]);
            in->status = V4LBUF_AVAILABLE;
            break;
        default:
            av_log(in->pool->log_ctx, AV_LOG_ERROR, "%s: Sorry, memory type %i not supported", __func__, in->pool->memory);
            return AVERROR_PATCHWELCOME;
    }
    return 0;
}

static int v4lbuf_to_avpkt(AVPacket *pkt, V4LBuffer *avbuf) {
    int ret;
    av_free_packet(pkt);
    if(ret = v4l2bufref(avbuf, 0, &pkt->buf))
        return ret;

    pkt->data = pkt->buf->data;
    if(V4L2_TYPE_IS_MULTIPLANAR(avbuf->pool->type)) {
        pkt->size = avbuf->buf.m.planes[0].bytesused;
    } else
        pkt->size = avbuf->buf.bytesused;

    if(avbuf->buf.flags & V4L2_BUF_FLAG_KEYFRAME)
        pkt->flags |= AV_PKT_FLAG_KEY;

    pkt->pts = get_pts(avbuf);
    return 0;
}

static int v4lbuf_to_avframe(AVFrame *frame, V4LBuffer *avbuf) {
    int i, ret;
    av_frame_unref(frame);
    for(i=0; i<avbuf->num_planes; i++) {
        if(ret = v4l2bufref(avbuf, i, &frame->buf[i]))
            return ret;
        frame->data[i]     = frame->buf[i]->data;
        frame->linesize[i] = avbuf->linesize[i];
    }

    frame->format    = avbuf->pool->av_pix_fmt;
    frame->key_frame = !!(avbuf->buf.flags & V4L2_BUF_FLAG_KEYFRAME);
    frame->width     = avbuf->pool->width;
    frame->height    = avbuf->pool->height;
    frame->pts       = get_pts(avbuf);
    return 0;
}

static V4LBuffer* v4lbufpool_get_from_avframe(const AVFrame* frame, V4LBufferPool *p) {
    int i;
    for(i=0; i<p->num_buffers; i++) {
        if(V4LBUF_RET_USER == p->buffers[i].status) {
            switch(p->memory) {
                case V4L2_MEMORY_MMAP:
                    if(p->buffers[i].mmap_addr[0] == frame->buf[0]->data)
                        return &(p->buffers[i]);
                    break;
                default:
                    av_log(p->log_ctx, AV_LOG_ERROR, "%s: Sorry, memory type %i not supported\n", __func__, p->memory);
                    return NULL;
            }
        }
    }
    return NULL;
}

V4LBuffer* avpriv_v4lbufpool_getfreebuf(V4LBufferPool *p, const AVFrame *f, const AVPacket* pkt) {
    int i;
    V4LBuffer* ret = NULL;

#ifdef V4L_BUFFER_DEBUG
    av_log(p->log_ctx, AV_LOG_DEBUG, "Polling for a free buffer on %s\n", p->name);
#endif

    /* For input pools, we prefer to dequeue available buffers first. */
    if(V4L2_TYPE_IS_OUTPUT(p->type))
        while(dequeue_v4lbuf(p));

    if(f && (ret = v4lbufpool_get_from_avframe(f,p)))
        return ret;

    if(!ret)
        for(i=0; i<p->num_buffers; i++)
            if(p->buffers[i].status == V4LBUF_AVAILABLE)
                return &(p->buffers[i]);
    return ret;
}

int avpriv_set_stream_status(V4LBufferPool* bp, int cmd) {
    int ret;
    int type = bp->type;
    if((ret = bp->ioctl_f(bp->fd, cmd, &type)) < 0)
        return AVERROR(errno);
    bp->streamon = (cmd == VIDIOC_STREAMON);
    return 0;
}

int avpriv_v4l_enqueue_frame_or_pkt_or_buf(V4LBufferPool* bp, const AVFrame* f, const AVPacket* pkt, const uint8_t* buf, int buf_size) {
    V4LBuffer* avbuf = NULL;
    int ret;

    if(!f && !pkt && !buf) {
        av_log(bp->log_ctx, AV_LOG_ERROR, "%s: At least one of AVFrame*, AVPacket* or buf must be non NULL!\n", __func__);
        return AVERROR_BUG;
    }

    if(!(avbuf = avpriv_v4lbufpool_getfreebuf(bp, f, pkt))) {
        if(bp->memory == V4L2_MEMORY_MMAP)
            av_log(bp->log_ctx, AV_LOG_ERROR, "No free input buffer found\n");
        return AVERROR(ENOMEM);
    }

    if(f && (ret = avframe_to_v4lbuf(f, avbuf)))
        return ret;

    if(pkt && (ret = avpkt_to_v4lbuf(pkt, avbuf)))
        return ret;

    if(buf && (ret = buf2v4l(avbuf, 0, buf, buf_size, NULL)))
        return ret;

    if(ret = enqueue_v4lbuf(avbuf))
        return ret;

    return 0;
}

int avpriv_v4l_dequeue_frame_or_pkt(V4LBufferPool* bp, AVFrame* f, AVPacket* pkt) {
    V4LBuffer* avbuf = NULL;

    if((!f && !pkt) || (f && pkt)) {
        av_log(bp->log_ctx, AV_LOG_ERROR, "%s: Exactly one of AVFrame* or AVPacket* must be non NULL!\n", __func__);
        return AVERROR_BUG;
    }

    if(!(avbuf = dequeue_v4lbuf(bp)))
        return AVERROR(EAGAIN);

    if(f)
        return v4lbuf_to_avframe(f, avbuf);

    if(pkt)
        return v4lbuf_to_avpkt(pkt, avbuf);

    /* Impossible to get here */
    return AVERROR_BUG;
}

static inline int get_buffer_sanity_check(V4LBufferPool* bp, const char* func) {
    if(bp->memory != V4L2_MEMORY_MMAP) {
        av_log(bp->log_ctx, AV_LOG_ERROR, "Calling %s does not make sense with memory type %i\n", func, bp->memory);
        return AVERROR(EINVAL);
    }

    if(!V4L2_TYPE_IS_OUTPUT(bp->type)) {
        av_log(bp->log_ctx, AV_LOG_ERROR, "Calling %s does not make sense on capture pools.\n", func);
        return AVERROR(EINVAL);
    }

    return 0;
}

int av_v4l_buffer_pool_get_buffer_frame(void* priv_data, AVFrame* frame) {
    V4LBufferPool* bp = priv_data;
    V4LBuffer* avbuf = NULL;
    int ret;

    if(!frame)
        return AVERROR(EINVAL);

    if(ret = get_buffer_sanity_check(bp, __func__))
        return ret;

    if(bp->av_codec_id != AV_CODEC_ID_RAWVIDEO || bp->av_pix_fmt == AV_PIX_FMT_NONE) {
        av_log(bp->log_ctx, AV_LOG_ERROR, "Calling %s does not make sense on encoded streams.\n", __func__);
        return AVERROR(EINVAL);
    }

    if(!(avbuf = avpriv_v4lbufpool_getfreebuf(bp, NULL, NULL)))
        return AVERROR(ENOMEM);

    if(ret = v4lbuf_to_avframe(frame, avbuf)) {
        av_frame_unref(frame);
        return ret;
    }

    return 0;
}

int av_v4l_buffer_pool_get_buffer_packet(void* priv_data, AVPacket* pkt) {
    V4LBufferPool* bp = priv_data;
    V4LBuffer* avbuf = NULL;
    int ret;

    if(!pkt)
        return AVERROR(EINVAL);

    if(ret = get_buffer_sanity_check(bp, __func__))
        return ret;

    if(bp->av_codec_id == AV_CODEC_ID_RAWVIDEO || bp->av_pix_fmt != AV_PIX_FMT_NONE) {
        av_log(bp->log_ctx, AV_LOG_ERROR, "Calling %s does not make sense on raw streams.\n", __func__);
        return AVERROR(EINVAL);
    }

    if(!(avbuf = avpriv_v4lbufpool_getfreebuf(bp, NULL, NULL)))
        return AVERROR(ENOMEM);

    if(ret = v4lbuf_to_avpkt(pkt, avbuf)) {
        av_free_packet(pkt);
        return ret;
    }

    return 0;
}

int av_v4l_buffer_pool_make_pipe(void* src, void* dst) {
    int ret;
    V4LBuffer *ibuf = NULL, *obuf = NULL;
    V4LBufferPool *ibp = src;
    V4LBufferPool *obp = dst;

    /* Sanity checks */
    if(ibp->memory != V4L2_MEMORY_MMAP) {
        av_log(ibp->log_ctx, AV_LOG_ERROR, "Calling %s does not make sense with memory type %i for src\n", __func__, ibp->memory);
        return AVERROR(EINVAL);
    }

    if(!V4L2_TYPE_IS_OUTPUT(ibp->type)) {
        av_log(ibp->log_ctx, AV_LOG_ERROR, "Calling %s does not make sense withg src a capture pool.\n", __func__);
        return AVERROR(EINVAL);
    }

    if(obp->memory != V4L2_MEMORY_USERPTR) {
        av_log(obp->log_ctx, AV_LOG_ERROR, "Calling %s does not make sense with memory type %i for dst\n", __func__, obp->memory);
        return AVERROR(EINVAL);
    }

    if(V4L2_TYPE_IS_OUTPUT(obp->type)) {
        av_log(obp->log_ctx, AV_LOG_ERROR, "Calling %s does not make sense with dst an output pool.\n", __func__);
        return AVERROR(EINVAL);
    }

    /* Check for optimal settings */
    if(ibp->num_buffers != obp->num_buffers)
        av_log(ibp->log_ctx, AV_LOG_WARNING,
            "%s cannot perfectly match buffer pool sizes, please check your settings for optimal results.\n"
            "src has %i buffers, while dst has %i\n", __func__, ibp->num_buffers, obp->num_buffers);

    while( (ibuf = avpriv_v4lbufpool_getfreebuf(ibp, NULL, NULL)) &&
           (obuf = avpriv_v4lbufpool_getfreebuf(obp, NULL, NULL))) {
        if(ret = avbuf_to_avbuf(ibuf, obuf)) {
            av_log(ibp->log_ctx, AV_LOG_ERROR, "Failed to convert buffers\n");
            return ret;
        }

        if(ret = enqueue_v4lbuf(obuf)) {
            av_log(obp->log_ctx, AV_LOG_ERROR, "Failed to enqueue buffer\n");
            return ret;
        }

        ibuf->piped = obuf;
    }

    return 0;
}

static void buffer_callback(void *opaque, uint8_t *unused) {
    V4LBuffer* avbuf = opaque;
    if(--avbuf->ref_cnt<=0) {
        if(V4LBUF_IN_DRIVER != avbuf->status) {
            if(!V4L2_TYPE_IS_OUTPUT(avbuf->pool->type)) {
                enqueue_v4lbuf(avbuf);
            } else if(avbuf->piped) {
                if(avbuf_to_avbuf(avbuf, avbuf->piped) || enqueue_v4lbuf(avbuf->piped))
                    av_log(avbuf->pool->log_ctx, AV_LOG_FATAL, "Something went wrong when sending back buffer to its pipe...\n");
            } else {
                avbuf->status = V4LBUF_AVAILABLE;
            }
        }
    }
}
