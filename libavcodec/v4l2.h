/*
 * V4L2 bufferpools helper functions
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

#ifndef AVCODEC_V4L2_H
#define AVCODEC_V4L2_H

#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

/**
 * Allocates an AVFrame's buffers for feeding a V4L M2M device whose buffers
 * are mmaped in device memory.
 *
 * @param[in] priv_data A pointer to an output pool.
 * @param[in] frame A pre-allocated AVFrame. It will be unref'ed.
 * @return 0 in case of success, a negative value indicating the error otherwise.
 */
int av_v4l_buffer_pool_get_buffer_frame(void* priv_data, AVFrame* frame);

/**
 * Allocates an AVPacket's buffers for feeding a V4L M2M device whose buffers
 * are mmaped in device memory.
 *
 * @param[in] priv_data A pointer to an output pool.
 * @param[in] frame A pre-allocated AVPacket. Its buffers will be unref'ed.
 * @return 0 in case of success, a negative value indicating the error otherwise.
 */
int av_v4l_buffer_pool_get_buffer_packet(void* priv_data, AVPacket* pkt);

/**
 * Makes a "pipe" between src and dst buffer pools.
 *
 * @param[in] src A mmap'ed output buffer pool.
 * @param[in] dst A userptr capture buffer pool.
 *
 * src and dst should be configured to have the same number of buffers
 * for optimal performance.
 *
 * Buffers from src will be queued to dst, so that dst will write its results there.
 * When buffers from dst get fed back to src after processing them, they will be
 * queued without copy in src (an output queue).
 * When buffers are no longer used by src, they will be automatically queued back to dst.
 *
 * @return 0 in case of success, a negative value indicating the error otherwise.
 */
int av_v4l_buffer_pool_make_pipe(void* src, void* dst);

#endif // AVCODEC_V4L2_H
