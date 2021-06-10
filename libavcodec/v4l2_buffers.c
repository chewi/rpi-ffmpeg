/*
 * V4L2 buffer helper functions.
 *
 * Copyright (C) 2017 Alexis Ballier <aballier@gentoo.org>
 * Copyright (C) 2017 Jorge Ramirez <jorge.ramirez-ortiz@linaro.org>
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

#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include "libavcodec/avcodec.h"
#include "libavcodec/internal.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext.h"
#include "v4l2_context.h"
#include "v4l2_buffers.h"
#include "v4l2_m2m.h"
#include "weak_link.h"

#define USEC_PER_SEC 1000000
static const AVRational v4l2_timebase = { 1, USEC_PER_SEC };

static inline V4L2m2mContext *buf_to_m2mctx(V4L2Buffer *buf)
{
    return V4L2_TYPE_IS_OUTPUT(buf->context->type) ?
        container_of(buf->context, V4L2m2mContext, output) :
        container_of(buf->context, V4L2m2mContext, capture);
}

static inline AVCodecContext *logger(V4L2Buffer *buf)
{
    return buf_to_m2mctx(buf)->avctx;
}

static inline AVRational v4l2_get_timebase(V4L2Buffer *avbuf)
{
    V4L2m2mContext *s = buf_to_m2mctx(avbuf);
    const AVRational tb = s->avctx->pkt_timebase.num ?
        s->avctx->pkt_timebase :
        s->avctx->time_base;
    return tb.num && tb.den ? tb : v4l2_timebase;
}

static inline void v4l2_set_pts(V4L2Buffer *out, int64_t pts, int no_rescale)
{
    /* convert pts to v4l2 timebase */
    const int64_t v4l2_pts =
        no_rescale ? pts :
        pts == AV_NOPTS_VALUE ? 0 :
            av_rescale_q(pts, v4l2_get_timebase(out), v4l2_timebase);
    out->buf.timestamp.tv_usec = v4l2_pts % USEC_PER_SEC;
    out->buf.timestamp.tv_sec = v4l2_pts / USEC_PER_SEC;
}

static inline int64_t v4l2_get_pts(V4L2Buffer *avbuf, int no_rescale)
{
    /* convert pts back to encoder timebase */
    const int64_t v4l2_pts = (int64_t)avbuf->buf.timestamp.tv_sec * USEC_PER_SEC +
                        avbuf->buf.timestamp.tv_usec;

    return
        no_rescale ? v4l2_pts :
        v4l2_pts == 0 ? AV_NOPTS_VALUE :
            av_rescale_q(v4l2_pts, v4l2_timebase, v4l2_get_timebase(avbuf));
}

static void set_buf_length(V4L2Buffer *out, unsigned int plane, uint32_t bytesused, uint32_t length)
{
    if (V4L2_TYPE_IS_MULTIPLANAR(out->buf.type)) {
        out->planes[plane].bytesused = bytesused;
        out->planes[plane].length = length;
    } else {
        out->buf.bytesused = bytesused;
        out->buf.length = length;
    }
}

static enum AVColorPrimaries v4l2_get_color_primaries(V4L2Buffer *buf)
{
    enum v4l2_ycbcr_encoding ycbcr;
    enum v4l2_colorspace cs;

    cs = V4L2_TYPE_IS_MULTIPLANAR(buf->buf.type) ?
        buf->context->format.fmt.pix_mp.colorspace :
        buf->context->format.fmt.pix.colorspace;

    ycbcr = V4L2_TYPE_IS_MULTIPLANAR(buf->buf.type) ?
        buf->context->format.fmt.pix_mp.ycbcr_enc:
        buf->context->format.fmt.pix.ycbcr_enc;

    switch(ycbcr) {
    case V4L2_YCBCR_ENC_XV709:
    case V4L2_YCBCR_ENC_709: return AVCOL_PRI_BT709;
    case V4L2_YCBCR_ENC_XV601:
    case V4L2_YCBCR_ENC_601:return AVCOL_PRI_BT470M;
    default:
        break;
    }

    switch(cs) {
    case V4L2_COLORSPACE_470_SYSTEM_BG: return AVCOL_PRI_BT470BG;
    case V4L2_COLORSPACE_SMPTE170M: return AVCOL_PRI_SMPTE170M;
    case V4L2_COLORSPACE_SMPTE240M: return AVCOL_PRI_SMPTE240M;
    case V4L2_COLORSPACE_BT2020: return AVCOL_PRI_BT2020;
    default:
        break;
    }

    return AVCOL_PRI_UNSPECIFIED;
}

static enum AVColorRange v4l2_get_color_range(V4L2Buffer *buf)
{
    enum v4l2_quantization qt;

    qt = V4L2_TYPE_IS_MULTIPLANAR(buf->buf.type) ?
        buf->context->format.fmt.pix_mp.quantization :
        buf->context->format.fmt.pix.quantization;

    switch (qt) {
    case V4L2_QUANTIZATION_LIM_RANGE: return AVCOL_RANGE_MPEG;
    case V4L2_QUANTIZATION_FULL_RANGE: return AVCOL_RANGE_JPEG;
    default:
        break;
    }

     return AVCOL_RANGE_UNSPECIFIED;
}

static enum AVColorSpace v4l2_get_color_space(V4L2Buffer *buf)
{
    enum v4l2_ycbcr_encoding ycbcr;
    enum v4l2_colorspace cs;

    cs = V4L2_TYPE_IS_MULTIPLANAR(buf->buf.type) ?
        buf->context->format.fmt.pix_mp.colorspace :
        buf->context->format.fmt.pix.colorspace;

    ycbcr = V4L2_TYPE_IS_MULTIPLANAR(buf->buf.type) ?
        buf->context->format.fmt.pix_mp.ycbcr_enc:
        buf->context->format.fmt.pix.ycbcr_enc;

    switch(cs) {
    case V4L2_COLORSPACE_SRGB: return AVCOL_SPC_RGB;
    case V4L2_COLORSPACE_REC709: return AVCOL_SPC_BT709;
    case V4L2_COLORSPACE_470_SYSTEM_M: return AVCOL_SPC_FCC;
    case V4L2_COLORSPACE_470_SYSTEM_BG: return AVCOL_SPC_BT470BG;
    case V4L2_COLORSPACE_SMPTE170M: return AVCOL_SPC_SMPTE170M;
    case V4L2_COLORSPACE_SMPTE240M: return AVCOL_SPC_SMPTE240M;
    case V4L2_COLORSPACE_BT2020:
        if (ycbcr == V4L2_YCBCR_ENC_BT2020_CONST_LUM)
            return AVCOL_SPC_BT2020_CL;
        else
             return AVCOL_SPC_BT2020_NCL;
    default:
        break;
    }

    return AVCOL_SPC_UNSPECIFIED;
}

static enum AVColorTransferCharacteristic v4l2_get_color_trc(V4L2Buffer *buf)
{
    enum v4l2_ycbcr_encoding ycbcr;
    enum v4l2_xfer_func xfer;
    enum v4l2_colorspace cs;

    cs = V4L2_TYPE_IS_MULTIPLANAR(buf->buf.type) ?
        buf->context->format.fmt.pix_mp.colorspace :
        buf->context->format.fmt.pix.colorspace;

    ycbcr = V4L2_TYPE_IS_MULTIPLANAR(buf->buf.type) ?
        buf->context->format.fmt.pix_mp.ycbcr_enc:
        buf->context->format.fmt.pix.ycbcr_enc;

    xfer = V4L2_TYPE_IS_MULTIPLANAR(buf->buf.type) ?
        buf->context->format.fmt.pix_mp.xfer_func:
        buf->context->format.fmt.pix.xfer_func;

    switch (xfer) {
    case V4L2_XFER_FUNC_709: return AVCOL_TRC_BT709;
    case V4L2_XFER_FUNC_SRGB: return AVCOL_TRC_IEC61966_2_1;
    default:
        break;
    }

    switch (cs) {
    case V4L2_COLORSPACE_470_SYSTEM_M: return AVCOL_TRC_GAMMA22;
    case V4L2_COLORSPACE_470_SYSTEM_BG: return AVCOL_TRC_GAMMA28;
    case V4L2_COLORSPACE_SMPTE170M: return AVCOL_TRC_SMPTE170M;
    case V4L2_COLORSPACE_SMPTE240M: return AVCOL_TRC_SMPTE240M;
    default:
        break;
    }

    switch (ycbcr) {
    case V4L2_YCBCR_ENC_XV709:
    case V4L2_YCBCR_ENC_XV601: return AVCOL_TRC_BT1361_ECG;
    default:
        break;
    }

    return AVCOL_TRC_UNSPECIFIED;
}

static uint8_t * v4l2_get_drm_frame(V4L2Buffer *avbuf)
{
    AVDRMFrameDescriptor *drm_desc = &avbuf->drm_frame;
    AVDRMLayerDescriptor *layer;

    /* fill the DRM frame descriptor */
    drm_desc->nb_objects = avbuf->num_planes;
    drm_desc->nb_layers = 1;

    layer = &drm_desc->layers[0];
    layer->nb_planes = avbuf->num_planes;

    for (int i = 0; i < avbuf->num_planes; i++) {
        layer->planes[i].object_index = i;
        layer->planes[i].offset = 0;
        layer->planes[i].pitch = avbuf->plane_info[i].bytesperline;
    }

    switch (avbuf->context->av_pix_fmt) {
    case AV_PIX_FMT_YUYV422:

        layer->format = DRM_FORMAT_YUYV;
        layer->nb_planes = 1;

        break;

    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:

        layer->format = avbuf->context->av_pix_fmt == AV_PIX_FMT_NV12 ?
            DRM_FORMAT_NV12 : DRM_FORMAT_NV21;

        if (avbuf->num_planes > 1)
            break;

        layer->nb_planes = 2;

        layer->planes[1].object_index = 0;
        layer->planes[1].offset = avbuf->plane_info[0].bytesperline *
            avbuf->context->format.fmt.pix.height;
        layer->planes[1].pitch = avbuf->plane_info[0].bytesperline;
        break;

    case AV_PIX_FMT_YUV420P:

        layer->format = DRM_FORMAT_YUV420;

        if (avbuf->num_planes > 1)
            break;

        layer->nb_planes = 3;

        layer->planes[1].object_index = 0;
        layer->planes[1].offset = avbuf->plane_info[0].bytesperline *
            avbuf->context->format.fmt.pix.height;
        layer->planes[1].pitch = avbuf->plane_info[0].bytesperline >> 1;

        layer->planes[2].object_index = 0;
        layer->planes[2].offset = layer->planes[1].offset +
            ((avbuf->plane_info[0].bytesperline *
              avbuf->context->format.fmt.pix.height) >> 2);
        layer->planes[2].pitch = avbuf->plane_info[0].bytesperline >> 1;
        break;

    default:
        drm_desc->nb_layers = 0;
        break;
    }

    return (uint8_t *) drm_desc;
}

static void v4l2_free_bufref(void *opaque, uint8_t *data)
{
    AVBufferRef * bufref = (AVBufferRef *)data;
    V4L2Buffer *avbuf = (V4L2Buffer *)bufref->data;
    struct V4L2Context *ctx = ff_weak_link_lock(&avbuf->context_wl);

    if (ctx != NULL) {
        // Buffer still attached to context
        V4L2m2mContext *s = buf_to_m2mctx(avbuf);

        ff_mutex_lock(&ctx->lock);

        avbuf->status = V4L2BUF_AVAILABLE;

        if (s->draining && V4L2_TYPE_IS_OUTPUT(ctx->type)) {
            av_log(logger(avbuf), AV_LOG_DEBUG, "%s: Buffer avail\n", ctx->name);
            /* no need to queue more buffers to the driver */
        }
        else if (ctx->streamon) {
            av_log(logger(avbuf), AV_LOG_DEBUG, "%s: Buffer requeue\n", ctx->name);
            avbuf->buf.timestamp.tv_sec = 0;
            avbuf->buf.timestamp.tv_usec = 0;
            ff_v4l2_buffer_enqueue(avbuf);  // will set to IN_DRIVER
        }
        else {
            av_log(logger(avbuf), AV_LOG_DEBUG, "%s: Buffer freed but streamoff\n", ctx->name);
        }

        ff_mutex_unlock(&ctx->lock);
    }

    ff_weak_link_unlock(avbuf->context_wl);
    av_buffer_unref(&bufref);
}

static int v4l2_buffer_export_drm(V4L2Buffer* avbuf)
{
    struct v4l2_exportbuffer expbuf;
    int i, ret;

    for (i = 0; i < avbuf->num_planes; i++) {
        memset(&expbuf, 0, sizeof(expbuf));

        expbuf.index = avbuf->buf.index;
        expbuf.type = avbuf->buf.type;
        expbuf.plane = i;

        ret = ioctl(buf_to_m2mctx(avbuf)->fd, VIDIOC_EXPBUF, &expbuf);
        if (ret < 0)
            return AVERROR(errno);

        if (V4L2_TYPE_IS_MULTIPLANAR(avbuf->buf.type)) {
            /* drm frame */
            avbuf->drm_frame.objects[i].size = avbuf->buf.m.planes[i].length;
            avbuf->drm_frame.objects[i].fd = expbuf.fd;
            avbuf->drm_frame.objects[i].format_modifier = DRM_FORMAT_MOD_LINEAR;
        } else {
            /* drm frame */
            avbuf->drm_frame.objects[0].size = avbuf->buf.length;
            avbuf->drm_frame.objects[0].fd = expbuf.fd;
            avbuf->drm_frame.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
        }
    }

    return 0;
}

static int v4l2_bufref_to_buf(V4L2Buffer *out, int plane, const uint8_t* data, int size, int offset)
{
    unsigned int bytesused, length;
    int rv = 0;

    if (plane >= out->num_planes)
        return AVERROR(EINVAL);

    length = out->plane_info[plane].length;
    bytesused = FFMIN(size+offset, length);

    if (size > length - offset) {
        size = length - offset;
        rv = AVERROR(ENOMEM);
    }

    memcpy((uint8_t*)out->plane_info[plane].mm_addr+offset, data, size);

    set_buf_length(out, plane, bytesused, length);

    return rv;
}

static AVBufferRef * wrap_avbuf(V4L2Buffer * const avbuf)
{
    AVBufferRef * bufref = av_buffer_ref(avbuf->context->bufrefs[avbuf->buf.index]);
    AVBufferRef * newbuf;

    if (!bufref)
        return NULL;

    newbuf = av_buffer_create((uint8_t *)bufref, sizeof(*bufref), v4l2_free_bufref, NULL, 0);
    if (newbuf == NULL)
        av_buffer_unref(&bufref);

    avbuf->status = V4L2BUF_RET_USER;
    return newbuf;
}

static int v4l2_buffer_buf_to_swframe(AVFrame *frame, V4L2Buffer *avbuf)
{
    int i;

    frame->format = avbuf->context->av_pix_fmt;

    frame->buf[0] = wrap_avbuf(avbuf);
    if (frame->buf[0] == NULL)
        return AVERROR(ENOMEM);

    if (buf_to_m2mctx(avbuf)->output_drm) {
        /* 1. get references to the actual data */
        frame->data[0] = (uint8_t *) v4l2_get_drm_frame(avbuf);
        frame->format = AV_PIX_FMT_DRM_PRIME;
        frame->hw_frames_ctx = av_buffer_ref(avbuf->context->frames_ref);
        return 0;
    }


    /* 1. get references to the actual data */
    for (i = 0; i < avbuf->num_planes; i++) {
        frame->data[i] = (uint8_t *)avbuf->plane_info[i].mm_addr + avbuf->planes[i].data_offset;
        frame->linesize[i] = avbuf->plane_info[i].bytesperline;
    }

    /* fixup special cases */
    switch (avbuf->context->av_pix_fmt) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
        if (avbuf->num_planes > 1)
            break;
        frame->linesize[1] = frame->linesize[0];
        frame->data[1] = frame->data[0] + frame->linesize[0] * ff_v4l2_get_format_height(&avbuf->context->format);
        break;

    case AV_PIX_FMT_YUV420P:
        if (avbuf->num_planes > 1)
            break;
        frame->linesize[1] = frame->linesize[0] / 2;
        frame->linesize[2] = frame->linesize[1];
        frame->data[1] = frame->data[0] + frame->linesize[0] * ff_v4l2_get_format_height(&avbuf->context->format);
        frame->data[2] = frame->data[1] + frame->linesize[1] * ff_v4l2_get_format_height(&avbuf->context->format) / 2;
        break;

    default:
        break;
    }

    return 0;
}

static void cpy_2d(uint8_t * dst, int dst_stride, const uint8_t * src, int src_stride, int w, int h)
{
    if (dst_stride == src_stride && w + 32 >= dst_stride) {
        memcpy(dst, src, dst_stride * h);
    }
    else {
        while (--h >= 0) {
            memcpy(dst, src, w);
            dst += dst_stride;
            src += src_stride;
        }
    }
}

static int is_chroma(const AVPixFmtDescriptor *desc, int i, int num_planes)
{
    return i != 0  && !(i == num_planes - 1 && (desc->flags & AV_PIX_FMT_FLAG_ALPHA));
}

static int v4l2_buffer_swframe_to_buf(const AVFrame *frame, V4L2Buffer *out)
{
    int i;
    int num_planes = 0;
    int pel_strides[4] = {0};

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);

    if ((desc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: HWACCEL cannot be copied\n", __func__);
        return -1;
    }

    for (i = 0; i != desc->nb_components; ++i) {
        if (desc->comp[i].plane >= num_planes)
            num_planes = desc->comp[i].plane + 1;
        pel_strides[desc->comp[i].plane] = desc->comp[i].step;
    }

    if (out->num_planes > 1) {
        if (num_planes != out->num_planes) {
            av_log(NULL, AV_LOG_ERROR, "%s: Num planes mismatch: %d != %d\n", __func__, num_planes, out->num_planes);
            return -1;
        }
        for (i = 0; i != num_planes; ++i) {
            int w = frame->width;
            int h = frame->height;
            if (is_chroma(desc, i, num_planes)) {
                w = AV_CEIL_RSHIFT(w, desc->log2_chroma_w);
                h = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
            }

            cpy_2d(out->plane_info[i].mm_addr, out->plane_info[i].bytesperline,
                   frame->data[i], frame->linesize[i],
                   w * pel_strides[i], h);
            set_buf_length(out, i, out->plane_info[i].bytesperline * h, out->plane_info[i].length);
        }
    }
    else
    {
        unsigned int offset = 0;

        for (i = 0; i != num_planes; ++i) {
            int w = frame->width;
            int h = frame->height;
            int dst_stride = out->plane_info[0].bytesperline;
            uint8_t * const dst = (uint8_t *)out->plane_info[0].mm_addr + offset;

            if (is_chroma(desc, i, num_planes)) {
                // Is chroma
                dst_stride >>= desc->log2_chroma_w;
                offset += dst_stride * (out->context->height >> desc->log2_chroma_h);
                w = AV_CEIL_RSHIFT(w, desc->log2_chroma_w);
                h = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
            }
            else {
                // Is luma or alpha
                offset += dst_stride * out->context->height;
            }
            if (offset > out->plane_info[0].length) {
                av_log(NULL, AV_LOG_ERROR, "%s: Plane total %d > buffer size %d\n", __func__, offset, out->plane_info[0].length);
                return -1;
            }

            cpy_2d(dst, dst_stride,
                   frame->data[i], frame->linesize[i],
                   w * pel_strides[i], h);
        }
        set_buf_length(out, 0, offset, out->plane_info[0].length);
    }
    return 0;
}

/******************************************************************************
 *
 *              V4L2Buffer interface
 *
 ******************************************************************************/

int ff_v4l2_buffer_avframe_to_buf(const AVFrame *frame, V4L2Buffer *out)
{
    v4l2_set_pts(out, frame->pts, 0);

    return v4l2_buffer_swframe_to_buf(frame, out);
}

int ff_v4l2_buffer_buf_to_avframe(AVFrame *frame, V4L2Buffer *avbuf, int no_rescale_pts)
{
    int ret;
    V4L2Context * const ctx = avbuf->context;

    av_frame_unref(frame);

    /* 1. get references to the actual data */
    ret = v4l2_buffer_buf_to_swframe(frame, avbuf);
    if (ret)
        return ret;

    /* 2. get frame information */
    frame->key_frame = !!(avbuf->buf.flags & V4L2_BUF_FLAG_KEYFRAME);
    frame->color_primaries = v4l2_get_color_primaries(avbuf);
    frame->colorspace = v4l2_get_color_space(avbuf);
    frame->color_range = v4l2_get_color_range(avbuf);
    frame->color_trc = v4l2_get_color_trc(avbuf);
    frame->pts = v4l2_get_pts(avbuf, no_rescale_pts);
    frame->pkt_dts = AV_NOPTS_VALUE;

    /* these values are updated also during re-init in v4l2_process_driver_event */
    frame->height = ctx->height;
    frame->width = ctx->width;
    frame->sample_aspect_ratio = ctx->sample_aspect_ratio;

    if (ctx->selection.height && ctx->selection.width) {
        frame->crop_left = ctx->selection.left < frame->width ? ctx->selection.left : 0;
        frame->crop_top  = ctx->selection.top < frame->height ? ctx->selection.top  : 0;
        frame->crop_right = ctx->selection.left + ctx->selection.width < frame->width ?
            frame->width - (ctx->selection.left + ctx->selection.width) : 0;
        frame->crop_bottom = ctx->selection.top + ctx->selection.height < frame->height ?
            frame->width - (ctx->selection.top + ctx->selection.height) : 0;
    }

    /* 3. report errors upstream */
    if (avbuf->buf.flags & V4L2_BUF_FLAG_ERROR) {
        av_log(logger(avbuf), AV_LOG_ERROR, "%s: driver decode error\n", avbuf->context->name);
        frame->decode_error_flags |= FF_DECODE_ERROR_INVALID_BITSTREAM;
    }

    return 0;
}

int ff_v4l2_buffer_buf_to_avpkt(AVPacket *pkt, V4L2Buffer *avbuf)
{
    av_log(logger(avbuf), AV_LOG_INFO, "%s\n", __func__);

    av_packet_unref(pkt);

    pkt->buf = wrap_avbuf(avbuf);
    if (pkt->buf == NULL)
        return AVERROR(ENOMEM);

    pkt->size = V4L2_TYPE_IS_MULTIPLANAR(avbuf->buf.type) ? avbuf->buf.m.planes[0].bytesused : avbuf->buf.bytesused;
    pkt->data = (uint8_t*)avbuf->plane_info[0].mm_addr + avbuf->planes[0].data_offset;

    if (avbuf->buf.flags & V4L2_BUF_FLAG_KEYFRAME)
        pkt->flags |= AV_PKT_FLAG_KEY;

    if (avbuf->buf.flags & V4L2_BUF_FLAG_ERROR) {
        av_log(logger(avbuf), AV_LOG_ERROR, "%s driver encode error\n", avbuf->context->name);
        pkt->flags |= AV_PKT_FLAG_CORRUPT;
    }

    pkt->dts = pkt->pts = v4l2_get_pts(avbuf, 0);

    return 0;
}

int ff_v4l2_buffer_avpkt_to_buf_ext(const AVPacket *pkt, V4L2Buffer *out,
                                    const void *extdata, size_t extlen, int no_rescale_pts)
{
    int ret;

    if (extlen) {
        ret = v4l2_bufref_to_buf(out, 0, extdata, extlen, 0);
        if (ret)
            return ret;
    }

    ret = v4l2_bufref_to_buf(out, 0, pkt->data, pkt->size, extlen);
    if (ret && ret != AVERROR(ENOMEM))
        return ret;

    v4l2_set_pts(out, pkt->pts, no_rescale_pts);

    if (pkt->flags & AV_PKT_FLAG_KEY)
        out->flags = V4L2_BUF_FLAG_KEYFRAME;

    return ret;
}

int ff_v4l2_buffer_avpkt_to_buf(const AVPacket *pkt, V4L2Buffer *out)
{
    return ff_v4l2_buffer_avpkt_to_buf_ext(pkt, out, NULL, 0, 0);
}


static void v4l2_buffer_buffer_free(void *opaque, uint8_t *data)
{
    V4L2Buffer * const avbuf = (V4L2Buffer *)data;
    int i;

    for (i = 0; i != FF_ARRAY_ELEMS(avbuf->plane_info); ++i) {
        struct V4L2Plane_info *p = avbuf->plane_info + i;
        if (p->mm_addr != NULL)
            munmap(p->mm_addr, p->length);
    }

    for (i = 0; i != FF_ARRAY_ELEMS(avbuf->drm_frame.objects); ++i) {
        if (avbuf->drm_frame.objects[i].fd != -1)
            close(avbuf->drm_frame.objects[i].fd);
    }

    ff_weak_link_unref(&avbuf->context_wl);

    av_free(avbuf);
}


int ff_v4l2_buffer_initialize(AVBufferRef ** pbufref, int index, V4L2Context *ctx)
{
    int ret, i;
    V4L2Buffer * const avbuf = av_mallocz(sizeof(*avbuf));
    AVBufferRef * bufref;

    *pbufref = NULL;
    if (avbuf == NULL)
        return AVERROR(ENOMEM);

    bufref = av_buffer_create((uint8_t*)avbuf, sizeof(*avbuf), v4l2_buffer_buffer_free, NULL, 0);
    if (bufref == NULL) {
        av_free(avbuf);
        return AVERROR(ENOMEM);
    }

    avbuf->context = ctx;
    avbuf->buf.memory = V4L2_MEMORY_MMAP;
    avbuf->buf.type = ctx->type;
    avbuf->buf.index = index;

    for (i = 0; i != FF_ARRAY_ELEMS(avbuf->drm_frame.objects); ++i) {
        avbuf->drm_frame.objects[i].fd = -1;
    }

    avbuf->context_wl = ff_weak_link_ref(ctx->wl_master);

    if (V4L2_TYPE_IS_MULTIPLANAR(ctx->type)) {
        avbuf->buf.length = VIDEO_MAX_PLANES;
        avbuf->buf.m.planes = avbuf->planes;
    }

    ret = ioctl(buf_to_m2mctx(avbuf)->fd, VIDIOC_QUERYBUF, &avbuf->buf);
    if (ret < 0)
        goto fail;

    if (V4L2_TYPE_IS_MULTIPLANAR(ctx->type)) {
        avbuf->num_planes = 0;
        /* in MP, the V4L2 API states that buf.length means num_planes */
        for (i = 0; i < avbuf->buf.length; i++) {
            if (avbuf->buf.m.planes[i].length)
                avbuf->num_planes++;
        }
    } else
        avbuf->num_planes = 1;

    for (i = 0; i < avbuf->num_planes; i++) {

        avbuf->plane_info[i].bytesperline = V4L2_TYPE_IS_MULTIPLANAR(ctx->type) ?
            ctx->format.fmt.pix_mp.plane_fmt[i].bytesperline :
            ctx->format.fmt.pix.bytesperline;

        if (V4L2_TYPE_IS_MULTIPLANAR(ctx->type)) {
            avbuf->plane_info[i].length = avbuf->buf.m.planes[i].length;

            if ((V4L2_TYPE_IS_OUTPUT(ctx->type) && buf_to_m2mctx(avbuf)->output_drm) ||
                !buf_to_m2mctx(avbuf)->output_drm) {
                avbuf->plane_info[i].mm_addr = mmap(NULL, avbuf->buf.m.planes[i].length,
                                               PROT_READ | PROT_WRITE, MAP_SHARED,
                                               buf_to_m2mctx(avbuf)->fd, avbuf->buf.m.planes[i].m.mem_offset);
            }
        } else {
            avbuf->plane_info[i].length = avbuf->buf.length;

            if ((V4L2_TYPE_IS_OUTPUT(ctx->type) && buf_to_m2mctx(avbuf)->output_drm) ||
                !buf_to_m2mctx(avbuf)->output_drm) {
                avbuf->plane_info[i].mm_addr = mmap(NULL, avbuf->buf.length,
                                               PROT_READ | PROT_WRITE, MAP_SHARED,
                                               buf_to_m2mctx(avbuf)->fd, avbuf->buf.m.offset);
            }
        }

        if (avbuf->plane_info[i].mm_addr == MAP_FAILED) {
            avbuf->plane_info[i].mm_addr = NULL;
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    avbuf->status = V4L2BUF_AVAILABLE;

    if (V4L2_TYPE_IS_MULTIPLANAR(ctx->type)) {
        avbuf->buf.m.planes = avbuf->planes;
        avbuf->buf.length   = avbuf->num_planes;

    } else {
        avbuf->buf.bytesused = avbuf->planes[0].bytesused;
        avbuf->buf.length    = avbuf->planes[0].length;
    }

    if (!V4L2_TYPE_IS_OUTPUT(ctx->type)) {
        if (buf_to_m2mctx(avbuf)->output_drm) {
            ret = v4l2_buffer_export_drm(avbuf);
            if (ret)
                    goto fail;
        }
    }

    *pbufref = bufref;
    return 0;

fail:
    av_buffer_unref(&bufref);
    return ret;
}

int ff_v4l2_buffer_enqueue(V4L2Buffer* avbuf)
{
    int ret;

    avbuf->buf.flags = avbuf->flags;

    if (avbuf->buf.timestamp.tv_sec || avbuf->buf.timestamp.tv_usec) {
        av_log(logger(avbuf), AV_LOG_DEBUG, "--- %s pre VIDIOC_QBUF: index %d, ts=%ld.%06ld count=%d\n",
               avbuf->context->name, avbuf->buf.index,
               avbuf->buf.timestamp.tv_sec, avbuf->buf.timestamp.tv_usec,
               avbuf->context->q_count);
    }

    ret = ioctl(buf_to_m2mctx(avbuf)->fd, VIDIOC_QBUF, &avbuf->buf);
    if (ret < 0) {
        int err = errno;
        av_log(logger(avbuf), AV_LOG_ERROR, "--- %s VIDIOC_QBUF: index %d FAIL err %d (%s)\n",
               avbuf->context->name, avbuf->buf.index,
               err, strerror(err));
        return AVERROR(err);
    }

    ++avbuf->context->q_count;
    av_log(logger(avbuf), AV_LOG_DEBUG, "--- %s VIDIOC_QBUF: index %d, ts=%ld.%06ld count=%d\n",
           avbuf->context->name, avbuf->buf.index,
           avbuf->buf.timestamp.tv_sec, avbuf->buf.timestamp.tv_usec,
           avbuf->context->q_count);

    avbuf->status = V4L2BUF_IN_DRIVER;

    return 0;
}
