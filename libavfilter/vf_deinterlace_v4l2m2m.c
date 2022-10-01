/*
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
 * deinterlace video filter - V4L2 M2M
 */

#include <drm_fourcc.h>

#include <linux/videodev2.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "config.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#define FF_INTERNAL_FIELDS 1
#include "framequeue.h"
#include "filters.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "scale_eval.h"
#include "video.h"

#ifndef DRM_FORMAT_P030
#define DRM_FORMAT_P030 fourcc_code('P', '0', '3', '0') /* 2x2 subsampled Cr:Cb plane 10 bits per channel packed */
#endif

// V4L2_PIX_FMT_NV12_10_COL128 and V4L2_PIX_FMT_NV12_COL128 should be defined
// in drm_fourcc.h hopefully will be sometime in the future but until then...
#ifndef V4L2_PIX_FMT_NV12_10_COL128
#define V4L2_PIX_FMT_NV12_10_COL128 v4l2_fourcc('N', 'C', '3', '0')
#endif

#ifndef V4L2_PIX_FMT_NV12_COL128
#define V4L2_PIX_FMT_NV12_COL128 v4l2_fourcc('N', 'C', '1', '2') /* 12  Y/CbCr 4:2:0 128 pixel wide column */
#endif

typedef struct V4L2Queue V4L2Queue;
typedef struct DeintV4L2M2MContextShared DeintV4L2M2MContextShared;

typedef enum filter_type_v4l2_e
{
    FILTER_V4L2_DEINTERLACE = 1,
    FILTER_V4L2_SCALE,
} filter_type_v4l2_t;

typedef struct V4L2Buffer {
    int enqueued;
    int reenqueue;
    struct v4l2_buffer buffer;
    AVFrame frame;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    int num_planes;
    AVDRMFrameDescriptor drm_frame;
    V4L2Queue *q;
} V4L2Buffer;

typedef struct V4L2Queue {
    struct v4l2_format format;
    struct v4l2_selection sel;
    int eos;
    int num_buffers;
    V4L2Buffer *buffers;
    const char * name;
    DeintV4L2M2MContextShared *ctx;
} V4L2Queue;

typedef struct pts_stats_s
{
    void * logctx;
    const char * name;  // For debug
    unsigned int last_count;
    unsigned int last_interval;
    int64_t last_pts;
} pts_stats_t;

#define PTS_TRACK_SIZE 32
typedef struct pts_track_el_s
{
    uint32_t n;
    unsigned int interval;
    AVFrame * props;
} pts_track_el_t;

typedef struct pts_track_s
{
    uint32_t n;
    uint32_t last_n;
    int got_2;
    void * logctx;
    pts_stats_t stats;
    pts_track_el_t a[PTS_TRACK_SIZE];
} pts_track_t;

typedef enum drain_state_e
{
    DRAIN_NONE = 0,     // Not draining
    DRAIN_TIMEOUT,      // Drain until normal timeout setup yields no frame
    DRAIN_LAST,         // Drain with long timeout last_frame in received on output expected
    DRAIN_EOS,          // Drain with long timeout EOS expected
    DRAIN_DONE          // Drained
} drain_state_t;

typedef struct DeintV4L2M2MContextShared {
    void * logctx;  // For logging - will be NULL when done
    filter_type_v4l2_t filter_type;

    int fd;
    int done;   // fd closed - awating all refs dropped
    int width;
    int height;

    int drain;          // EOS received (inlink status)
    drain_state_t drain_state;
    int64_t drain_pts;  // PTS associated with inline status

    unsigned int frames_rx;
    unsigned int frames_tx;

    // from options
    int output_width;
    int output_height;
    enum AVPixelFormat output_format;

    int has_enc_stop;
    // We expect to get exactly the same number of frames out as we put in
    // We can drain by matching input to output
    int one_to_one;

    int orig_width;
    int orig_height;
    atomic_uint refcount;

    AVBufferRef *hw_frames_ctx;

    unsigned int field_order;

    pts_track_t track;

    V4L2Queue output;
    V4L2Queue capture;
} DeintV4L2M2MContextShared;

typedef struct DeintV4L2M2MContext {
    const AVClass *class;

    DeintV4L2M2MContextShared *shared;

    char * w_expr;
    char * h_expr;
    char * output_format_string;;

    int force_original_aspect_ratio;
    int force_divisible_by;

    char *colour_primaries_string;
    char *colour_transfer_string;
    char *colour_matrix_string;
    int   colour_range;
    char *chroma_location_string;

    enum AVColorPrimaries colour_primaries;
    enum AVColorTransferCharacteristic colour_transfer;
    enum AVColorSpace colour_matrix;
    enum AVChromaLocation chroma_location;
} DeintV4L2M2MContext;


static inline int drain_frame_expected(const drain_state_t d)
{
    return d == DRAIN_EOS || d == DRAIN_LAST;
}

// These just list the ones we know we can cope with
static uint32_t
fmt_av_to_v4l2(const enum AVPixelFormat avfmt)
{
    switch (avfmt) {
    case AV_PIX_FMT_YUV420P:
        return V4L2_PIX_FMT_YUV420;
    case AV_PIX_FMT_NV12:
        return V4L2_PIX_FMT_NV12;
#if CONFIG_SAND
    case AV_PIX_FMT_RPI4_8:
    case AV_PIX_FMT_SAND128:
        return V4L2_PIX_FMT_NV12_COL128;
#endif
    default:
        break;
    }
    return 0;
}

static enum AVPixelFormat
fmt_v4l2_to_av(const uint32_t pixfmt)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_YUV420:
        return AV_PIX_FMT_YUV420P;
    case V4L2_PIX_FMT_NV12:
        return AV_PIX_FMT_NV12;
#if CONFIG_SAND
    case V4L2_PIX_FMT_NV12_COL128:
        return AV_PIX_FMT_RPI4_8;
#endif
    default:
        break;
    }
    return AV_PIX_FMT_NONE;
}

static unsigned int pts_stats_interval(const pts_stats_t * const stats)
{
    return stats->last_interval;
}

// Pick 64 for max last count - that is >1sec at 60fps
#define STATS_LAST_COUNT_MAX 64
#define STATS_INTERVAL_MAX (1 << 30)
static void pts_stats_add(pts_stats_t * const stats, int64_t pts)
{
    if (pts == AV_NOPTS_VALUE || pts == stats->last_pts) {
        if (stats->last_count < STATS_LAST_COUNT_MAX)
            ++stats->last_count;
        return;
    }

    if (stats->last_pts != AV_NOPTS_VALUE) {
        const int64_t interval = pts - stats->last_pts;

        if (interval < 0 || interval >= STATS_INTERVAL_MAX ||
            stats->last_count >= STATS_LAST_COUNT_MAX) {
            if (stats->last_interval != 0)
                av_log(stats->logctx, AV_LOG_DEBUG, "%s: %s: Bad interval: %" PRId64 "/%d\n",
                       __func__, stats->name, interval, stats->last_count);
            stats->last_interval = 0;
        }
        else {
            const int64_t frame_time = interval / (int64_t)stats->last_count;

            if (frame_time != stats->last_interval)
                av_log(stats->logctx, AV_LOG_DEBUG, "%s: %s: New interval: %u->%" PRId64 "/%d=%" PRId64 "\n",
                       __func__, stats->name, stats->last_interval, interval, stats->last_count, frame_time);
            stats->last_interval = frame_time;
        }
    }

    stats->last_pts = pts;
    stats->last_count = 1;
}

static void pts_stats_init(pts_stats_t * const stats, void * logctx, const char * name)
{
    *stats = (pts_stats_t){
        .logctx = logctx,
        .name = name,
        .last_count = 1,
        .last_interval = 0,
        .last_pts = AV_NOPTS_VALUE
    };
}

static inline uint32_t pts_track_next_n(pts_track_t * const trk)
{
    if (++trk->n == 0)
        trk->n = 1;
    return trk->n;
}

static int pts_track_get_frame(pts_track_t * const trk, const struct timeval tv, AVFrame * const dst)
{
    uint32_t n = (uint32_t)(tv.tv_usec / 2 + tv.tv_sec * 500000);
    pts_track_el_t * t;

    // As a first guess assume that n==0 means last frame
    if (n == 0) {
        n = trk->last_n;
        if (n == 0)
            goto fail;
    }

    t = trk->a + (n & (PTS_TRACK_SIZE - 1));

    if (t->n != n) {
        av_log(trk->logctx, AV_LOG_ERROR, "%s: track failure: got %u, expected %u\n", __func__, n, trk->n);
        goto fail;
    }

    // 1st frame is simple - just believe it
    if (n != trk->last_n) {
        trk->last_n = n;
        trk->got_2 = 0;
        return av_frame_copy_props(dst, t->props);
    }

    // Only believe in a single interpolated frame
    if (trk->got_2)
        goto fail;
    trk->got_2 = 1;

    av_frame_copy_props(dst, t->props);


    // If we can't guess - don't
    if (t->interval == 0) {
        dst->best_effort_timestamp = AV_NOPTS_VALUE;
        dst->pts = AV_NOPTS_VALUE;
        dst->pkt_dts = AV_NOPTS_VALUE;
    }
    else {
        if (dst->best_effort_timestamp != AV_NOPTS_VALUE)
            dst->best_effort_timestamp += t->interval / 2;
        if (dst->pts != AV_NOPTS_VALUE)
            dst->pts += t->interval / 2;
        if (dst->pkt_dts != AV_NOPTS_VALUE)
            dst->pkt_dts += t->interval / 2;
    }

    return 0;

fail:
    trk->last_n = 0;
    trk->got_2 = 0;
    dst->pts = AV_NOPTS_VALUE;
    dst->pkt_dts = AV_NOPTS_VALUE;
    return 0;
}

// We are only ever expecting in-order frames so nothing more clever is required
static unsigned int
pts_track_count(const pts_track_t * const trk)
{
    return (trk->n - trk->last_n) & (PTS_TRACK_SIZE - 1);
}

static struct timeval pts_track_add_frame(pts_track_t * const trk, const AVFrame * const src)
{
    const uint32_t n = pts_track_next_n(trk);
    pts_track_el_t * const t = trk->a + (n & (PTS_TRACK_SIZE - 1));

    pts_stats_add(&trk->stats, src->pts);

    t->n = n;
    t->interval = pts_stats_interval(&trk->stats); // guess that next interval is the same as the last
    av_frame_unref(t->props);
    av_frame_copy_props(t->props, src);

    // We now know what the previous interval was, rather than having to guess,
    // so set it.  There is a better than decent chance that this is before
    // we use it.
    if (t->interval != 0) {
        pts_track_el_t * const prev_t = trk->a + ((n - 1) & (PTS_TRACK_SIZE - 1));
        prev_t->interval = t->interval;
    }

    // In case deinterlace interpolates frames use every other usec
    return (struct timeval){.tv_sec = n / 500000, .tv_usec = (n % 500000) * 2};
}

static void pts_track_uninit(pts_track_t * const trk)
{
    unsigned int i;
    for (i = 0; i != PTS_TRACK_SIZE; ++i) {
        trk->a[i].n = 0;
        av_frame_free(&trk->a[i].props);
    }
}

static int pts_track_init(pts_track_t * const trk, void *logctx)
{
    unsigned int i;
    trk->n = 1;
    pts_stats_init(&trk->stats, logctx, "track");
    for (i = 0; i != PTS_TRACK_SIZE; ++i) {
        trk->a[i].n = 0;
        if ((trk->a[i].props = av_frame_alloc()) == NULL) {
            pts_track_uninit(trk);
            return AVERROR(ENOMEM);
        }
    }
    return 0;
}

static inline uint32_t
fmt_bpl(const struct v4l2_format * const fmt, const unsigned int plane_n)
{
    return V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ? fmt->fmt.pix_mp.plane_fmt[plane_n].bytesperline : fmt->fmt.pix.bytesperline;
}

static inline uint32_t
fmt_height(const struct v4l2_format * const fmt)
{
    return V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ? fmt->fmt.pix_mp.height : fmt->fmt.pix.height;
}

static inline uint32_t
fmt_width(const struct v4l2_format * const fmt)
{
    return V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ? fmt->fmt.pix_mp.width : fmt->fmt.pix.width;
}

static inline uint32_t
fmt_pixelformat(const struct v4l2_format * const fmt)
{
    return V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ? fmt->fmt.pix_mp.pixelformat : fmt->fmt.pix.pixelformat;
}

static inline uint32_t
buf_bytesused0(const struct v4l2_buffer * const buf)
{
    return V4L2_TYPE_IS_MULTIPLANAR(buf->type) ? buf->m.planes[0].bytesused : buf->bytesused;
}

static void
init_format(V4L2Queue * const q, const uint32_t format_type)
{
    memset(&q->format, 0, sizeof(q->format));
    memset(&q->sel,    0, sizeof(q->sel));
    q->format.type = format_type;
    q->sel.type    = format_type;
}

static int deint_v4l2m2m_prepare_context(DeintV4L2M2MContextShared *ctx)
{
    struct v4l2_capability cap;
    int ret;

    memset(&cap, 0, sizeof(cap));
    ret = ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0)
        return ret;

    if (ctx->filter_type == FILTER_V4L2_SCALE &&
        strcmp("bcm2835-codec-isp", cap.card) != 0)
    {
        av_log(ctx->logctx, AV_LOG_DEBUG, "Not ISP\n");
        return AVERROR(EINVAL);
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "No streaming\n");
        return AVERROR(EINVAL);
    }

    if (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) {
        init_format(&ctx->capture, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        init_format(&ctx->output,  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    }
    else if (cap.capabilities & V4L2_CAP_VIDEO_M2M) {
        init_format(&ctx->capture, V4L2_BUF_TYPE_VIDEO_CAPTURE);
        init_format(&ctx->output,  V4L2_BUF_TYPE_VIDEO_OUTPUT);
    }
    else {
        av_log(ctx->logctx, AV_LOG_DEBUG, "Not M2M\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

// Just use for probe - doesn't modify q format
static int deint_v4l2m2m_try_format(V4L2Queue *queue, const uint32_t width, const uint32_t height, const enum AVPixelFormat avfmt)
{
    struct v4l2_format fmt         = {.type = queue->format.type};
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    int ret, field;
    // Pick YUV to test with if not otherwise specified
    uint32_t pixelformat = avfmt == AV_PIX_FMT_NONE ? V4L2_PIX_FMT_YUV420 : fmt_av_to_v4l2(avfmt);
    enum AVPixelFormat r_avfmt;


    ret = ioctl(ctx->fd, VIDIOC_G_FMT, &fmt);
    if (ret)
        av_log(ctx->logctx, AV_LOG_ERROR, "VIDIOC_G_FMT failed: %d\n", ret);

    if (ctx->filter_type == FILTER_V4L2_DEINTERLACE && V4L2_TYPE_IS_OUTPUT(fmt.type))
        field = V4L2_FIELD_INTERLACED_TB;
    else
        field = V4L2_FIELD_NONE;

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt.type)) {
        fmt.fmt.pix_mp.pixelformat = pixelformat;
        fmt.fmt.pix_mp.field = field;
        fmt.fmt.pix_mp.width = width;
        fmt.fmt.pix_mp.height = height;
    } else {
        fmt.fmt.pix.pixelformat = pixelformat;
        fmt.fmt.pix.field = field;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
    }

    av_log(ctx->logctx, AV_LOG_TRACE, "%s: Trying format for type %d, wxh: %dx%d, fmt: %08x, size %u bpl %u pre\n", __func__,
         fmt.type, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
         fmt.fmt.pix_mp.pixelformat,
         fmt.fmt.pix_mp.plane_fmt[0].sizeimage, fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

    ret = ioctl(ctx->fd, VIDIOC_TRY_FMT, &fmt);
    if (ret)
        return AVERROR(EINVAL);

    av_log(ctx->logctx, AV_LOG_TRACE, "%s: Trying format for type %d, wxh: %dx%d, fmt: %08x, size %u bpl %u post\n", __func__,
         fmt.type, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
         fmt.fmt.pix_mp.pixelformat,
         fmt.fmt.pix_mp.plane_fmt[0].sizeimage, fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

    r_avfmt = fmt_v4l2_to_av(fmt_pixelformat(&fmt));
    if (r_avfmt != avfmt && avfmt != AV_PIX_FMT_NONE) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "Unable to set format %s on %s port\n", av_get_pix_fmt_name(avfmt), V4L2_TYPE_IS_CAPTURE(fmt.type) ? "dest" : "src");
        return AVERROR(EINVAL);
    }
    if (r_avfmt == AV_PIX_FMT_NONE) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "No supported format on %s port\n", V4L2_TYPE_IS_CAPTURE(fmt.type) ? "dest" : "src");
        return AVERROR(EINVAL);
    }

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt.type)) {
        if (fmt.fmt.pix_mp.field != field) {
            av_log(ctx->logctx, AV_LOG_DEBUG, "format not supported for type %d\n", fmt.type);

            return AVERROR(EINVAL);
        }
    } else {
        if (fmt.fmt.pix.field != field) {
            av_log(ctx->logctx, AV_LOG_DEBUG, "format not supported for type %d\n", fmt.type);

            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static int
do_s_fmt(V4L2Queue * const q)
{
    DeintV4L2M2MContextShared * const ctx = q->ctx;
    const uint32_t pixelformat = fmt_pixelformat(&q->format);
    int ret;

    ret = ioctl(ctx->fd, VIDIOC_S_FMT, &q->format);
    if (ret) {
        ret = AVERROR(errno);
        av_log(ctx->logctx, AV_LOG_ERROR, "VIDIOC_S_FMT failed: %s\n", av_err2str(ret));
        return ret;
    }

    if (pixelformat != fmt_pixelformat(&q->format)) {
        av_log(ctx->logctx, AV_LOG_ERROR, "Format not supported: %s; S_FMT returned %s\n", av_fourcc2str(pixelformat), av_fourcc2str(fmt_pixelformat(&q->format)));
        return AVERROR(EINVAL);
    }

    q->sel.target = V4L2_TYPE_IS_OUTPUT(q->sel.type) ? V4L2_SEL_TGT_CROP : V4L2_SEL_TGT_COMPOSE,
    q->sel.flags  = V4L2_TYPE_IS_OUTPUT(q->sel.type) ? V4L2_SEL_FLAG_LE : V4L2_SEL_FLAG_GE;

    ret = ioctl(ctx->fd, VIDIOC_S_SELECTION, &q->sel);
    if (ret) {
        ret = AVERROR(errno);
        av_log(ctx->logctx, AV_LOG_WARNING, "VIDIOC_S_SELECTION failed: %s\n", av_err2str(ret));
    }

    return 0;
}

static void
set_fmt_color(struct v4l2_format *const fmt,
               const enum AVColorPrimaries avcp,
               const enum AVColorSpace avcs,
               const enum AVColorTransferCharacteristic avxc)
{
    enum v4l2_ycbcr_encoding ycbcr = V4L2_YCBCR_ENC_DEFAULT;
    enum v4l2_colorspace cs = V4L2_COLORSPACE_DEFAULT;
    enum v4l2_xfer_func xfer = V4L2_XFER_FUNC_DEFAULT;

    switch (avcp) {
    case AVCOL_PRI_BT709:
        cs = V4L2_COLORSPACE_REC709;
        ycbcr = V4L2_YCBCR_ENC_709;
        break;
    case AVCOL_PRI_BT470M:
        cs = V4L2_COLORSPACE_470_SYSTEM_M;
        ycbcr = V4L2_YCBCR_ENC_601;
        break;
    case AVCOL_PRI_BT470BG:
        cs = V4L2_COLORSPACE_470_SYSTEM_BG;
        break;
    case AVCOL_PRI_SMPTE170M:
        cs = V4L2_COLORSPACE_SMPTE170M;
        break;
    case AVCOL_PRI_SMPTE240M:
        cs = V4L2_COLORSPACE_SMPTE240M;
        break;
    case AVCOL_PRI_BT2020:
        cs = V4L2_COLORSPACE_BT2020;
        break;
    case AVCOL_PRI_SMPTE428:
    case AVCOL_PRI_SMPTE431:
    case AVCOL_PRI_SMPTE432:
    case AVCOL_PRI_EBU3213:
    case AVCOL_PRI_RESERVED:
    case AVCOL_PRI_FILM:
    case AVCOL_PRI_UNSPECIFIED:
    default:
        break;
    }

    switch (avcs) {
    case AVCOL_SPC_RGB:
        cs = V4L2_COLORSPACE_SRGB;
        break;
    case AVCOL_SPC_BT709:
        cs = V4L2_COLORSPACE_REC709;
        break;
    case AVCOL_SPC_FCC:
        cs = V4L2_COLORSPACE_470_SYSTEM_M;
        break;
    case AVCOL_SPC_BT470BG:
        cs = V4L2_COLORSPACE_470_SYSTEM_BG;
        break;
    case AVCOL_SPC_SMPTE170M:
        cs = V4L2_COLORSPACE_SMPTE170M;
        break;
    case AVCOL_SPC_SMPTE240M:
        cs = V4L2_COLORSPACE_SMPTE240M;
        break;
    case AVCOL_SPC_BT2020_CL:
        cs = V4L2_COLORSPACE_BT2020;
        ycbcr = V4L2_YCBCR_ENC_BT2020_CONST_LUM;
        break;
    case AVCOL_SPC_BT2020_NCL:
        cs = V4L2_COLORSPACE_BT2020;
        break;
    default:
        break;
    }

    switch (xfer) {
    case AVCOL_TRC_BT709:
        xfer = V4L2_XFER_FUNC_709;
        break;
    case AVCOL_TRC_IEC61966_2_1:
        xfer = V4L2_XFER_FUNC_SRGB;
        break;
    case AVCOL_TRC_SMPTE240M:
        xfer = V4L2_XFER_FUNC_SMPTE240M;
        break;
    case AVCOL_TRC_SMPTE2084:
        xfer = V4L2_XFER_FUNC_SMPTE2084;
        break;
    default:
        break;
    }

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        fmt->fmt.pix_mp.colorspace = cs;
        fmt->fmt.pix_mp.ycbcr_enc = ycbcr;
        fmt->fmt.pix_mp.xfer_func = xfer;
    } else {
        fmt->fmt.pix.colorspace = cs;
        fmt->fmt.pix.ycbcr_enc = ycbcr;
        fmt->fmt.pix.xfer_func = xfer;
    }
}

static void
set_fmt_color_range(struct v4l2_format *const fmt, const enum AVColorRange avcr)
{
    const enum v4l2_quantization q =
        avcr == AVCOL_RANGE_MPEG ? V4L2_QUANTIZATION_LIM_RANGE :
        avcr == AVCOL_RANGE_JPEG ? V4L2_QUANTIZATION_FULL_RANGE :
            V4L2_QUANTIZATION_DEFAULT;

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        fmt->fmt.pix_mp.quantization = q;
    } else {
        fmt->fmt.pix.quantization = q;
    }
}

static enum AVColorPrimaries get_color_primaries(const struct v4l2_format *const fmt)
{
    enum v4l2_ycbcr_encoding ycbcr;
    enum v4l2_colorspace cs;

    cs = V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ?
        fmt->fmt.pix_mp.colorspace :
        fmt->fmt.pix.colorspace;

    ycbcr = V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ?
        fmt->fmt.pix_mp.ycbcr_enc:
        fmt->fmt.pix.ycbcr_enc;

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

static enum AVColorSpace get_color_space(const struct v4l2_format *const fmt)
{
    enum v4l2_ycbcr_encoding ycbcr;
    enum v4l2_colorspace cs;

    cs = V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ?
        fmt->fmt.pix_mp.colorspace :
        fmt->fmt.pix.colorspace;

    ycbcr = V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ?
        fmt->fmt.pix_mp.ycbcr_enc:
        fmt->fmt.pix.ycbcr_enc;

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

static enum AVColorTransferCharacteristic get_color_trc(const struct v4l2_format *const fmt)
{
    enum v4l2_ycbcr_encoding ycbcr;
    enum v4l2_xfer_func xfer;
    enum v4l2_colorspace cs;

    cs = V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ?
        fmt->fmt.pix_mp.colorspace :
        fmt->fmt.pix.colorspace;

    ycbcr = V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ?
        fmt->fmt.pix_mp.ycbcr_enc:
        fmt->fmt.pix.ycbcr_enc;

    xfer = V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ?
        fmt->fmt.pix_mp.xfer_func:
        fmt->fmt.pix.xfer_func;

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

static enum AVColorRange get_color_range(const struct v4l2_format *const fmt)
{
    enum v4l2_quantization qt;

    qt = V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ?
        fmt->fmt.pix_mp.quantization :
        fmt->fmt.pix.quantization;

    switch (qt) {
    case V4L2_QUANTIZATION_LIM_RANGE: return AVCOL_RANGE_MPEG;
    case V4L2_QUANTIZATION_FULL_RANGE: return AVCOL_RANGE_JPEG;
    default:
        break;
    }

     return AVCOL_RANGE_UNSPECIFIED;
}

static int set_src_fmt(V4L2Queue * const q, const AVFrame * const frame)
{
    struct v4l2_format *const format = &q->format;
    const AVDRMFrameDescriptor *const src = (const AVDRMFrameDescriptor *)frame->data[0];

    const uint32_t drm_fmt = src->layers[0].format;
    // Treat INVALID as LINEAR
    const uint64_t mod = src->objects[0].format_modifier == DRM_FORMAT_MOD_INVALID ?
        DRM_FORMAT_MOD_LINEAR : src->objects[0].format_modifier;
    uint32_t pix_fmt = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t bpl = src->layers[0].planes[0].pitch;

    // We really don't expect multiple layers
    // All formats that we currently cope with are single object

    if (src->nb_layers != 1 || src->nb_objects != 1)
        return AVERROR(EINVAL);

    switch (drm_fmt) {
        case DRM_FORMAT_YUV420:
            if (mod == DRM_FORMAT_MOD_LINEAR) {
                if (src->layers[0].nb_planes != 3)
                    break;
                pix_fmt = V4L2_PIX_FMT_YUV420;
                h = src->layers[0].planes[1].offset / bpl;
                w = bpl;
            }
            break;

        case DRM_FORMAT_NV12:
            if (mod == DRM_FORMAT_MOD_LINEAR) {
                if (src->layers[0].nb_planes != 2)
                    break;
                pix_fmt = V4L2_PIX_FMT_NV12;
                h = src->layers[0].planes[1].offset / bpl;
                w = bpl;
            }
#if CONFIG_SAND
            else if (fourcc_mod_broadcom_mod(mod) == DRM_FORMAT_MOD_BROADCOM_SAND128) {
                if (src->layers[0].nb_planes != 2)
                    break;
                pix_fmt = V4L2_PIX_FMT_NV12_COL128;
                w = bpl;
                h = src->layers[0].planes[1].offset / 128;
                bpl = fourcc_mod_broadcom_param(mod);
            }
#endif
            break;

        case DRM_FORMAT_P030:
#if CONFIG_SAND
            if (fourcc_mod_broadcom_mod(mod) == DRM_FORMAT_MOD_BROADCOM_SAND128) {
                if (src->layers[0].nb_planes != 2)
                    break;
                pix_fmt =  V4L2_PIX_FMT_NV12_10_COL128;
                w = bpl / 2;  // Matching lie to how we construct this
                h = src->layers[0].planes[1].offset / 128;
                bpl = fourcc_mod_broadcom_param(mod);
            }
#endif
            break;

        default:
            break;
    }

    if (!pix_fmt)
        return AVERROR(EINVAL);

    if (V4L2_TYPE_IS_MULTIPLANAR(format->type)) {
        struct v4l2_pix_format_mplane *const pix = &format->fmt.pix_mp;

        pix->width = w;
        pix->height = h;
        pix->pixelformat = pix_fmt;
        pix->plane_fmt[0].bytesperline = bpl;
        pix->num_planes = 1;
    }
    else {
        struct v4l2_pix_format *const pix = &format->fmt.pix;

        pix->width = w;
        pix->height = h;
        pix->pixelformat = pix_fmt;
        pix->bytesperline = bpl;
    }

    set_fmt_color(format, frame->color_primaries, frame->colorspace, frame->color_trc);
    set_fmt_color_range(format, frame->color_range);

    q->sel.r.width = frame->width - (frame->crop_left + frame->crop_right);
    q->sel.r.height = frame->height - (frame->crop_top + frame->crop_bottom);
    q->sel.r.left = frame->crop_left;
    q->sel.r.top = frame->crop_top;

    return 0;
}


static int set_dst_format(DeintV4L2M2MContext * const priv, V4L2Queue *queue, uint32_t pixelformat, uint32_t field, int width, int height)
{
    struct v4l2_format * const fmt   = &queue->format;
    struct v4l2_selection *const sel = &queue->sel;

    memset(&fmt->fmt, 0, sizeof(fmt->fmt));

    // Align w/h to 16 here in case there are alignment requirements at the next
    // stage of the filter chain (also RPi deinterlace setup is bust and this
    // fixes it)
    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        fmt->fmt.pix_mp.pixelformat = pixelformat;
        fmt->fmt.pix_mp.field = field;
        fmt->fmt.pix_mp.width = FFALIGN(width, 16);
        fmt->fmt.pix_mp.height = FFALIGN(height, 16);
    } else {
        fmt->fmt.pix.pixelformat = pixelformat;
        fmt->fmt.pix.field = field;
        fmt->fmt.pix.width = FFALIGN(width, 16);
        fmt->fmt.pix.height = FFALIGN(height, 16);
    }

    set_fmt_color(fmt, priv->colour_primaries, priv->colour_matrix, priv->colour_transfer);
    set_fmt_color_range(fmt, priv->colour_range);

    sel->r.width = width;
    sel->r.height = height;
    sel->r.left = 0;
    sel->r.top = 0;

    return do_s_fmt(queue);
}

static int deint_v4l2m2m_probe_device(DeintV4L2M2MContextShared *ctx, char *node)
{
    int ret;

    ctx->fd = open(node, O_RDWR | O_NONBLOCK, 0);
    if (ctx->fd < 0)
        return AVERROR(errno);

    ret = deint_v4l2m2m_prepare_context(ctx);
    if (ret) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "Failed to prepare context\n");
        goto fail;
    }

    ret = deint_v4l2m2m_try_format(&ctx->capture, ctx->output_width, ctx->output_height, ctx->output_format);
    if (ret) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "Failed to try dst format\n");
        goto fail;
    }

    ret = deint_v4l2m2m_try_format(&ctx->output, ctx->width, ctx->height, AV_PIX_FMT_NONE);
    if (ret) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "Failed to try src format\n");
        goto fail;
    }

    return 0;

fail:
    close(ctx->fd);
    ctx->fd = -1;

    return ret;
}

static int deint_v4l2m2m_find_device(DeintV4L2M2MContextShared *ctx)
{
    int ret = AVERROR(EINVAL);
    struct dirent *entry;
    char node[PATH_MAX];
    DIR *dirp;

    dirp = opendir("/dev");
    if (!dirp)
        return AVERROR(errno);

    for (entry = readdir(dirp); entry; entry = readdir(dirp)) {

        if (strncmp(entry->d_name, "video", 5))
            continue;

        snprintf(node, sizeof(node), "/dev/%s", entry->d_name);
        av_log(ctx->logctx, AV_LOG_DEBUG, "probing device %s\n", node);
        ret = deint_v4l2m2m_probe_device(ctx, node);
        if (!ret)
            break;
    }

    closedir(dirp);

    if (ret) {
        av_log(ctx->logctx, AV_LOG_ERROR, "Could not find a valid device\n");
        ctx->fd = -1;

        return ret;
    }

    av_log(ctx->logctx, AV_LOG_INFO, "Using device %s\n", node);

    return 0;
}

static int deint_v4l2m2m_enqueue_buffer(V4L2Buffer *buf)
{
    int ret;

    ret = ioctl(buf->q->ctx->fd, VIDIOC_QBUF, &buf->buffer);
    if (ret < 0)
        return AVERROR(errno);

    buf->enqueued = 1;

    return 0;
}

static void
drm_frame_init(AVDRMFrameDescriptor * const d)
{
    unsigned int i;
    for (i = 0; i != AV_DRM_MAX_PLANES; ++i) {
        d->objects[i].fd = -1;
    }
}

static void
drm_frame_uninit(AVDRMFrameDescriptor * const d)
{
    unsigned int i;
    for (i = 0; i != d->nb_objects; ++i) {
        if (d->objects[i].fd != -1) {
            close(d->objects[i].fd);
            d->objects[i].fd = -1;
        }
    }
}

static void
avbufs_delete(V4L2Buffer** ppavbufs, const unsigned int n)
{
    unsigned int i;
    V4L2Buffer* const avbufs = *ppavbufs;

    if (avbufs == NULL)
        return;
    *ppavbufs = NULL;

    for (i = 0; i != n; ++i) {
        V4L2Buffer* const avbuf = avbufs + i;
        drm_frame_uninit(&avbuf->drm_frame);
    }

    av_free(avbufs);
}

static int v4l2_buffer_export_drm(V4L2Queue * const q, V4L2Buffer * const avbuf)
{
    struct v4l2_exportbuffer expbuf;
    int i, ret;
    uint64_t mod = DRM_FORMAT_MOD_LINEAR;

    AVDRMFrameDescriptor * const drm_desc = &avbuf->drm_frame;
    AVDRMLayerDescriptor * const layer = &drm_desc->layers[0];
    const struct v4l2_format *const fmt = &q->format;
    const uint32_t height = fmt_height(fmt);
    ptrdiff_t bpl0;

    /* fill the DRM frame descriptor */
    drm_desc->nb_layers = 1;
    layer->nb_planes = avbuf->num_planes;

    for (int i = 0; i < avbuf->num_planes; i++) {
        layer->planes[i].object_index = i;
        layer->planes[i].offset = 0;
        layer->planes[i].pitch = fmt_bpl(fmt, i);
    }
    bpl0 = layer->planes[0].pitch;

    switch (fmt_pixelformat(fmt)) {
#if CONFIG_SAND
        case V4L2_PIX_FMT_NV12_COL128:
            mod = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(bpl0);
            layer->format = V4L2_PIX_FMT_NV12;

            if (avbuf->num_planes > 1)
                break;

            layer->nb_planes = 2;
            layer->planes[1].object_index = 0;
            layer->planes[1].offset = height * 128;
            layer->planes[0].pitch = fmt_width(fmt);
            layer->planes[1].pitch = layer->planes[0].pitch;
            break;
#endif

        case DRM_FORMAT_NV12:
            layer->format = V4L2_PIX_FMT_NV12;

            if (avbuf->num_planes > 1)
                break;

            layer->nb_planes = 2;
            layer->planes[1].object_index = 0;
            layer->planes[1].offset = bpl0 * height;
            layer->planes[1].pitch = bpl0;
            break;

        case V4L2_PIX_FMT_YUV420:
            layer->format = DRM_FORMAT_YUV420;

            if (avbuf->num_planes > 1)
                break;

            layer->nb_planes = 3;
            layer->planes[1].object_index = 0;
            layer->planes[1].offset = bpl0 * height;
            layer->planes[1].pitch = bpl0 / 2;
            layer->planes[2].object_index = 0;
            layer->planes[2].offset = layer->planes[1].offset + ((bpl0 * height) / 4);
            layer->planes[2].pitch = bpl0 / 2;
            break;

        default:
            drm_desc->nb_layers = 0;
            return AVERROR(EINVAL);
    }

    drm_desc->nb_objects = 0;
    for (i = 0; i < avbuf->num_planes; i++) {
        memset(&expbuf, 0, sizeof(expbuf));

        expbuf.index = avbuf->buffer.index;
        expbuf.type = avbuf->buffer.type;
        expbuf.plane = i;

        ret = ioctl(avbuf->q->ctx->fd, VIDIOC_EXPBUF, &expbuf);
        if (ret < 0)
            return AVERROR(errno);

        drm_desc->objects[i].size = V4L2_TYPE_IS_MULTIPLANAR(avbuf->buffer.type) ?
            avbuf->buffer.m.planes[i].length : avbuf->buffer.length;
        drm_desc->objects[i].fd = expbuf.fd;
        drm_desc->objects[i].format_modifier = mod;
        drm_desc->nb_objects = i + 1;
    }

    return 0;
}

static int deint_v4l2m2m_allocate_buffers(V4L2Queue *queue)
{
    struct v4l2_format *fmt = &queue->format;
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    struct v4l2_requestbuffers req;
    int ret, i, multiplanar;
    uint32_t memory;

    memory = V4L2_TYPE_IS_OUTPUT(fmt->type) ?
        V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;

    multiplanar = V4L2_TYPE_IS_MULTIPLANAR(fmt->type);

    memset(&req, 0, sizeof(req));
    req.count = queue->num_buffers;
    req.memory = memory;
    req.type = fmt->type;

    ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        av_log(ctx->logctx, AV_LOG_ERROR, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));

        return AVERROR(errno);
    }

    queue->num_buffers = req.count;
    queue->buffers = av_mallocz(queue->num_buffers * sizeof(V4L2Buffer));
    if (!queue->buffers) {
        av_log(ctx->logctx, AV_LOG_ERROR, "malloc enomem\n");

        return AVERROR(ENOMEM);
    }

    for (i = 0; i < queue->num_buffers; i++) {
        V4L2Buffer * const buf = &queue->buffers[i];

        buf->enqueued = 0;
        buf->q = queue;

        buf->buffer.type = fmt->type;
        buf->buffer.memory = memory;
        buf->buffer.index = i;

        if (multiplanar) {
            buf->buffer.length = VIDEO_MAX_PLANES;
            buf->buffer.m.planes = buf->planes;
        }

        drm_frame_init(&buf->drm_frame);
    }

    for (i = 0; i < queue->num_buffers; i++) {
        V4L2Buffer * const buf = &queue->buffers[i];

        ret = ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf->buffer);
        if (ret < 0) {
            ret = AVERROR(errno);

            goto fail;
        }

        buf->num_planes = multiplanar ? buf->buffer.length : 1;

        if (!V4L2_TYPE_IS_OUTPUT(fmt->type)) {
            ret = deint_v4l2m2m_enqueue_buffer(buf);
            if (ret)
                goto fail;

            ret = v4l2_buffer_export_drm(queue, buf);
            if (ret)
                goto fail;
        }
    }

    return 0;

fail:
    avbufs_delete(&queue->buffers, queue->num_buffers);
    queue->num_buffers = 0;
    return ret;
}

static int deint_v4l2m2m_streamon(V4L2Queue *queue)
{
    DeintV4L2M2MContextShared * const ctx = queue->ctx;
    int type = queue->format.type;
    int ret;

    ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
    av_log(ctx->logctx, AV_LOG_DEBUG, "%s: type:%d ret:%d errno:%d\n", __func__, type, ret, AVERROR(errno));
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

static int deint_v4l2m2m_streamoff(V4L2Queue *queue)
{
    DeintV4L2M2MContextShared * const ctx = queue->ctx;
    int type = queue->format.type;
    int ret;

    ret = ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    av_log(ctx->logctx, AV_LOG_DEBUG, "%s: type:%d ret:%d errno:%d\n", __func__, type, ret, AVERROR(errno));
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

// timeout in ms
static V4L2Buffer* deint_v4l2m2m_dequeue_buffer(V4L2Queue *queue, int timeout)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    struct v4l2_buffer buf = { 0 };
    V4L2Buffer* avbuf = NULL;
    struct pollfd pfd;
    short events;
    int ret;

    if (V4L2_TYPE_IS_OUTPUT(queue->format.type))
        events =  POLLOUT | POLLWRNORM;
    else
        events = POLLIN | POLLRDNORM;

    pfd.events = events;
    pfd.fd = ctx->fd;

    for (;;) {
        ret = poll(&pfd, 1, timeout);
        if (ret > 0)
            break;
        if (errno == EINTR)
            continue;
        return NULL;
    }

    if (pfd.revents & POLLERR)
        return NULL;

    if (pfd.revents & events) {
        memset(&buf, 0, sizeof(buf));
        buf.memory = V4L2_MEMORY_MMAP;
        buf.type = queue->format.type;
        if (V4L2_TYPE_IS_MULTIPLANAR(queue->format.type)) {
            memset(planes, 0, sizeof(planes));
            buf.length = VIDEO_MAX_PLANES;
            buf.m.planes = planes;
        }

        ret = ioctl(ctx->fd, VIDIOC_DQBUF, &buf);
        if (ret) {
            if (errno != EAGAIN)
                av_log(ctx->logctx, AV_LOG_DEBUG, "VIDIOC_DQBUF, errno (%s)\n",
                       av_err2str(AVERROR(errno)));
            return NULL;
        }

        avbuf = &queue->buffers[buf.index];
        avbuf->enqueued = 0;
        avbuf->buffer = buf;
        if (V4L2_TYPE_IS_MULTIPLANAR(queue->format.type)) {
            memcpy(avbuf->planes, planes, sizeof(planes));
            avbuf->buffer.m.planes = avbuf->planes;
        }
        return avbuf;
    }

    return NULL;
}

static V4L2Buffer *deint_v4l2m2m_find_free_buf(V4L2Queue *queue)
{
    int i;
    V4L2Buffer *buf = NULL;

    for (i = 0; i < queue->num_buffers; i++)
        if (!queue->buffers[i].enqueued) {
            buf = &queue->buffers[i];
            break;
        }
    return buf;
}

static void deint_v4l2m2m_unref_queued(V4L2Queue *queue)
{
    int i;
    V4L2Buffer *buf = NULL;

    if (!queue || !queue->buffers)
        return;
    for (i = 0; i < queue->num_buffers; i++) {
        buf = &queue->buffers[i];
        if (queue->buffers[i].enqueued)
            av_frame_unref(&buf->frame);
    }
}

static void recycle_q(V4L2Queue * const queue)
{
    V4L2Buffer* avbuf;
    while (avbuf = deint_v4l2m2m_dequeue_buffer(queue, 0), avbuf) {
        av_frame_unref(&avbuf->frame);
    }
}

static int count_enqueued(V4L2Queue *queue)
{
    int i;
    int n = 0;

    if (queue->buffers == NULL)
        return 0;

    for (i = 0; i < queue->num_buffers; i++)
        if (queue->buffers[i].enqueued)
            ++n;
    return n;
}

static int deint_v4l2m2m_enqueue_frame(V4L2Queue * const queue, AVFrame * const frame)
{
    DeintV4L2M2MContextShared *const ctx = queue->ctx;
    AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor *)frame->data[0];
    V4L2Buffer *buf;
    int i;

    if (V4L2_TYPE_IS_OUTPUT(queue->format.type))
        recycle_q(queue);

    buf = deint_v4l2m2m_find_free_buf(queue);
    if (!buf) {
        av_log(ctx->logctx, AV_LOG_ERROR, "%s: error %d finding free buf\n", __func__, 0);
        return AVERROR(EAGAIN);
    }
    if (V4L2_TYPE_IS_MULTIPLANAR(buf->buffer.type))
        for (i = 0; i < drm_desc->nb_objects; i++)
            buf->buffer.m.planes[i].m.fd = drm_desc->objects[i].fd;
    else
        buf->buffer.m.fd = drm_desc->objects[0].fd;

    buf->buffer.field = !frame->interlaced_frame ? V4L2_FIELD_NONE :
        frame->top_field_first ? V4L2_FIELD_INTERLACED_TB :
            V4L2_FIELD_INTERLACED_BT;

    if (ctx->field_order != buf->buffer.field) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "%s: Field changed: %d->%d\n", __func__, ctx->field_order, buf->buffer.field);
        ctx->field_order = buf->buffer.field;
    }

    buf->buffer.timestamp = pts_track_add_frame(&ctx->track, frame);

    buf->drm_frame.objects[0].fd = drm_desc->objects[0].fd;

    av_frame_move_ref(&buf->frame, frame);

    return deint_v4l2m2m_enqueue_buffer(buf);
}

static void deint_v4l2m2m_destroy_context(DeintV4L2M2MContextShared *ctx)
{
    if (atomic_fetch_sub(&ctx->refcount, 1) == 1) {
        V4L2Queue *capture = &ctx->capture;
        V4L2Queue *output  = &ctx->output;

        av_log(NULL, AV_LOG_DEBUG, "%s - destroying context\n", __func__);

        if (ctx->fd >= 0) {
            deint_v4l2m2m_streamoff(capture);
            deint_v4l2m2m_streamoff(output);
        }

        avbufs_delete(&capture->buffers, capture->num_buffers);

        deint_v4l2m2m_unref_queued(output);

        av_buffer_unref(&ctx->hw_frames_ctx);

        if (capture->buffers)
            av_free(capture->buffers);

        if (output->buffers)
            av_free(output->buffers);

        if (ctx->fd >= 0) {
            close(ctx->fd);
            ctx->fd = -1;
        }

        av_free(ctx);
    }
}

static void v4l2_free_buffer(void *opaque, uint8_t *unused)
{
    V4L2Buffer *buf                = opaque;
    DeintV4L2M2MContextShared *ctx = buf->q->ctx;

    if (!ctx->done)
        deint_v4l2m2m_enqueue_buffer(buf);

    deint_v4l2m2m_destroy_context(ctx);
}

// timeout in ms
static int deint_v4l2m2m_dequeue_frame(V4L2Queue *queue, AVFrame* frame, int timeout)
{
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    V4L2Buffer* avbuf;
    enum AVColorPrimaries color_primaries;
    enum AVColorSpace colorspace;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorRange color_range;

    av_log(ctx->logctx, AV_LOG_TRACE, "<<< %s\n", __func__);

    if (queue->eos) {
        av_log(ctx->logctx, AV_LOG_TRACE, ">>> %s: EOS\n", __func__);
        return AVERROR_EOF;
    }

    avbuf = deint_v4l2m2m_dequeue_buffer(queue, timeout);
    if (!avbuf) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "%s: No buffer to dequeue (timeout=%d)\n", __func__, timeout);
        return AVERROR(EAGAIN);
    }

    if (V4L2_TYPE_IS_CAPTURE(avbuf->buffer.type)) {
        if ((avbuf->buffer.flags & V4L2_BUF_FLAG_LAST) != 0)
            queue->eos = 1;
        if (buf_bytesused0(&avbuf->buffer) == 0)
            return queue->eos ? AVERROR_EOF : AVERROR(EINVAL);
    }

    // Fill in PTS and anciliary info from src frame
    pts_track_get_frame(&ctx->track, avbuf->buffer.timestamp, frame);

    frame->buf[0] = av_buffer_create((uint8_t *) &avbuf->drm_frame,
                            sizeof(avbuf->drm_frame), v4l2_free_buffer,
                            avbuf, AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        av_log(ctx->logctx, AV_LOG_ERROR, "%s: error %d creating buffer\n", __func__, 0);
        return AVERROR(ENOMEM);
    }

    atomic_fetch_add(&ctx->refcount, 1);

    frame->data[0] = (uint8_t *)&avbuf->drm_frame;
    frame->format = AV_PIX_FMT_DRM_PRIME;
    if (ctx->hw_frames_ctx)
        frame->hw_frames_ctx = av_buffer_ref(ctx->hw_frames_ctx);
    frame->height = ctx->output_height;
    frame->width = ctx->output_width;

    color_primaries = get_color_primaries(&ctx->capture.format);
    colorspace      = get_color_space(&ctx->capture.format);
    color_trc       = get_color_trc(&ctx->capture.format);
    color_range     = get_color_range(&ctx->capture.format);

    // If the color parameters are unspecified by V4L2 then leave alone as they
    // will have been copied from src
    if (color_primaries != AVCOL_PRI_UNSPECIFIED)
        frame->color_primaries = color_primaries;
    if (colorspace != AVCOL_SPC_UNSPECIFIED)
        frame->colorspace = colorspace;
    if (color_trc != AVCOL_TRC_UNSPECIFIED)
        frame->color_trc = color_trc;
    if (color_range != AVCOL_RANGE_UNSPECIFIED)
        frame->color_range = color_range;

    if (ctx->filter_type == FILTER_V4L2_DEINTERLACE) {
        // Not interlaced now
        frame->interlaced_frame = 0;   // *** Fill in from dst buffer?
        frame->top_field_first = 0;
        // Pkt duration halved
        frame->pkt_duration /= 2;
    }

    if (avbuf->buffer.flags & V4L2_BUF_FLAG_ERROR) {
        av_log(ctx->logctx, AV_LOG_ERROR, "driver decode error\n");
        frame->decode_error_flags |= FF_DECODE_ERROR_INVALID_BITSTREAM;
    }

    av_log(ctx->logctx, AV_LOG_TRACE, ">>> %s: PTS=%"PRId64"\n", __func__, frame->pts);
    return 0;
}

static int deint_v4l2m2m_config_props(AVFilterLink *outlink)
{
    AVFilterLink *inlink           = outlink->src->inputs[0];
    AVFilterContext *avctx         = outlink->src;
    DeintV4L2M2MContext *priv      = avctx->priv;
    DeintV4L2M2MContextShared *ctx = priv->shared;
    int ret;

    ctx->height = avctx->inputs[0]->h;
    ctx->width = avctx->inputs[0]->w;

    if (ctx->filter_type == FILTER_V4L2_SCALE) {
        if ((ret = ff_scale_eval_dimensions(priv,
                                            priv->w_expr, priv->h_expr,
                                            inlink, outlink,
                                            &ctx->output_width, &ctx->output_height)) < 0)
            return ret;

        ff_scale_adjust_dimensions(inlink, &ctx->output_width, &ctx->output_height,
                                   priv->force_original_aspect_ratio, priv->force_divisible_by);
    }
    else {
        ctx->output_width  = ctx->width;
        ctx->output_height = ctx->height;
    }

    av_log(priv, AV_LOG_DEBUG, "%s: %dx%d->%dx%d FR: %d/%d->%d/%d\n", __func__,
           ctx->width, ctx->height, ctx->output_width, ctx->output_height,
           inlink->frame_rate.num, inlink->frame_rate.den, outlink->frame_rate.num, outlink->frame_rate.den);

    outlink->time_base           = inlink->time_base;
    outlink->w                   = ctx->output_width;
    outlink->h                   = ctx->output_height;
    outlink->format              = inlink->format;
    if (ctx->filter_type == FILTER_V4L2_DEINTERLACE && inlink->frame_rate.den != 0)
        outlink->frame_rate = (AVRational){inlink->frame_rate.num * 2, inlink->frame_rate.den};

    if (inlink->sample_aspect_ratio.num)
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink->w, outlink->w * inlink->h}, inlink->sample_aspect_ratio);
    else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    ret = deint_v4l2m2m_find_device(ctx);
    if (ret)
        return ret;

    if (inlink->hw_frames_ctx) {
        ctx->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);
        if (!ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }
    return 0;
}

static uint32_t desc_pixelformat(const AVDRMFrameDescriptor * const drm_desc)
{
    const uint64_t mod = drm_desc->objects[0].format_modifier;
    const int is_linear = (mod == DRM_FORMAT_MOD_LINEAR || mod == DRM_FORMAT_MOD_INVALID);

    // Only currently support single object things
    if (drm_desc->nb_objects != 1)
        return 0;

    switch (drm_desc->layers[0].format) {
    case DRM_FORMAT_YUV420:
        return is_linear ? V4L2_PIX_FMT_YUV420 : 0;
    case DRM_FORMAT_NV12:
        return is_linear ? V4L2_PIX_FMT_NV12 :
#if CONFIG_SAND
            fourcc_mod_broadcom_mod(mod) == DRM_FORMAT_MOD_BROADCOM_SAND128 ? V4L2_PIX_FMT_NV12_COL128 :
#endif
            0;
    default:
        break;
    }
    return 0;
}

static int deint_v4l2m2m_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *avctx         = link->dst;
    DeintV4L2M2MContext *priv      = avctx->priv;
    DeintV4L2M2MContextShared *ctx = priv->shared;
    V4L2Queue *capture             = &ctx->capture;
    V4L2Queue *output              = &ctx->output;
    int ret;

    av_log(priv, AV_LOG_DEBUG, "<<< %s: input pts: %"PRId64" (%"PRId64") field :%d interlaced: %d aspect:%d/%d\n",
          __func__, in->pts, AV_NOPTS_VALUE, in->top_field_first, in->interlaced_frame, in->sample_aspect_ratio.num, in->sample_aspect_ratio.den);
    av_log(priv, AV_LOG_DEBUG, "--- %s: in status in %d/ot %d; out status in %d/out %d\n", __func__,
           avctx->inputs[0]->status_in, avctx->inputs[0]->status_out, avctx->outputs[0]->status_in, avctx->outputs[0]->status_out);

    if (ctx->field_order == V4L2_FIELD_ANY) {
        const AVDRMFrameDescriptor * const drm_desc = (AVDRMFrameDescriptor *)in->data[0];
        uint32_t pixelformat = desc_pixelformat(drm_desc);

        if (pixelformat == 0) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported DRM format %s in %d objects, modifier %#" PRIx64 "\n",
                   av_fourcc2str(drm_desc->layers[0].format),
                   drm_desc->nb_objects, drm_desc->objects[0].format_modifier);
            return AVERROR(EINVAL);
        }

        ctx->orig_width = drm_desc->layers[0].planes[0].pitch;
        ctx->orig_height = drm_desc->layers[0].planes[1].offset / ctx->orig_width;

        av_log(priv, AV_LOG_DEBUG, "%s: %dx%d (%td,%td)\n", __func__, ctx->width, ctx->height,
           drm_desc->layers[0].planes[0].pitch, drm_desc->layers[0].planes[1].offset);

        if ((ret = set_src_fmt(output, in)) != 0) {
            av_log(avctx, AV_LOG_WARNING, "Unknown input DRM format: %s mod: %#" PRIx64 "\n",
                   av_fourcc2str(drm_desc->layers[0].format), drm_desc->objects[0].format_modifier);
            return ret;
        }

        ret = do_s_fmt(output);
        if (ret) {
            av_log(avctx, AV_LOG_WARNING, "Failed to set source format\n");
            return ret;
        }

        if (ctx->output_format != AV_PIX_FMT_NONE)
           pixelformat = fmt_av_to_v4l2(ctx->output_format);
        ret = set_dst_format(priv, capture, pixelformat, V4L2_FIELD_NONE, ctx->output_width, ctx->output_height);
        if (ret) {
            av_log(avctx, AV_LOG_WARNING, "Failed to set destination format\n");
            return ret;
        }

        ret = deint_v4l2m2m_allocate_buffers(capture);
        if (ret) {
            av_log(avctx, AV_LOG_WARNING, "Failed to allocate destination buffers\n");
            return ret;
        }

        ret = deint_v4l2m2m_streamon(capture);
        if (ret) {
            av_log(avctx, AV_LOG_WARNING, "Failed set destination streamon: %s\n", av_err2str(ret));
            return ret;
        }

        ret = deint_v4l2m2m_allocate_buffers(output);
        if (ret) {
            av_log(avctx, AV_LOG_WARNING, "Failed to allocate src buffers\n");
            return ret;
        }

        ret = deint_v4l2m2m_streamon(output);
        if (ret) {
            av_log(avctx, AV_LOG_WARNING, "Failed set src streamon: %s\n", av_err2str(ret));
            return ret;
        }

        if (in->top_field_first)
            ctx->field_order = V4L2_FIELD_INTERLACED_TB;
        else
            ctx->field_order = V4L2_FIELD_INTERLACED_BT;

        {
            struct v4l2_encoder_cmd ecmd = {
                .cmd = V4L2_ENC_CMD_STOP
            };
            ctx->has_enc_stop = 0;
            if (ioctl(ctx->fd, VIDIOC_TRY_ENCODER_CMD, &ecmd) == 0) {
                av_log(ctx->logctx, AV_LOG_DEBUG, "Test encode stop succeeded\n");
                ctx->has_enc_stop = 1;
            }
            else {
                av_log(ctx->logctx, AV_LOG_DEBUG, "Test encode stop fail: %s\n", av_err2str(AVERROR(errno)));
            }

        }
    }

    ret = deint_v4l2m2m_enqueue_frame(output, in);

    av_log(priv, AV_LOG_TRACE, ">>> %s: %s\n", __func__, av_err2str(ret));
    return ret;
}

static int
ack_inlink(AVFilterContext * const avctx, DeintV4L2M2MContextShared *const s,
           AVFilterLink * const inlink)
{
    int instatus;
    int64_t inpts;

    if (ff_inlink_acknowledge_status(inlink, &instatus, &inpts) <= 0)
        return 0;

    s->drain      = instatus;
    s->drain_pts  = inpts;
    s->drain_state = DRAIN_TIMEOUT;

    if (s->field_order == V4L2_FIELD_ANY) {  // Not yet started
        s->drain_state = DRAIN_DONE;
    }
    else if (s->one_to_one) {
        s->drain_state = DRAIN_LAST;
    }
    else if (s->has_enc_stop) {
        struct v4l2_encoder_cmd ecmd = {
            .cmd = V4L2_ENC_CMD_STOP
        };
        if (ioctl(s->fd, VIDIOC_ENCODER_CMD, &ecmd) == 0) {
            av_log(avctx->priv, AV_LOG_DEBUG, "Do Encode stop\n");
            s->drain_state = DRAIN_EOS;
        }
        else {
            av_log(avctx->priv, AV_LOG_WARNING, "Encode stop fail: %s\n", av_err2str(AVERROR(errno)));
        }
    }
    return 1;
}

static int deint_v4l2m2m_activate(AVFilterContext *avctx)
{
    DeintV4L2M2MContext * const priv = avctx->priv;
    DeintV4L2M2MContextShared *const s = priv->shared;
    AVFilterLink * const outlink = avctx->outputs[0];
    AVFilterLink * const inlink = avctx->inputs[0];
    int n = 0;
    int cn = 99;
    int did_something = 0;

    av_log(priv, AV_LOG_TRACE, "<<< %s\n", __func__);

    FF_FILTER_FORWARD_STATUS_BACK_ALL(outlink, avctx);

    ack_inlink(avctx, s, inlink);

    if (!ff_outlink_frame_wanted(outlink)) {
        av_log(priv, AV_LOG_TRACE, "%s: Not wanted out\n", __func__);
    }
    else if (s->field_order != V4L2_FIELD_ANY)  // Can't DQ if no setup!
    {
        AVFrame * frame = av_frame_alloc();
        int rv;

        recycle_q(&s->output);
        n = count_enqueued(&s->output);

        if (frame == NULL) {
            av_log(priv, AV_LOG_ERROR, "%s: error allocating frame\n", __func__);
            return AVERROR(ENOMEM);
        }

        rv = deint_v4l2m2m_dequeue_frame(&s->capture, frame,
                                         drain_frame_expected(s->drain_state) || n > 4 ? 300 : 0);
        if (rv != 0) {
            av_frame_free(&frame);
            if (rv == AVERROR_EOF) {
                av_log(priv, AV_LOG_DEBUG, "%s: --- DQ EOF\n", __func__);
                s->drain_state = DRAIN_DONE;
            }
            else if (rv == AVERROR(EAGAIN)) {
                if (s->drain_state != DRAIN_NONE) {
                    av_log(priv, AV_LOG_DEBUG, "%s: --- DQ empty - drain done\n", __func__);
                    s->drain_state = DRAIN_DONE;
                }
            }
            else {
                av_log(priv, AV_LOG_ERROR, ">>> %s: DQ fail: %s\n", __func__, av_err2str(rv));
                return rv;
            }
        }
        else {
            frame->interlaced_frame = 0;
            // frame is always consumed by filter_frame - even on error despite
            // a somewhat confusing comment in the header
            rv = ff_filter_frame(outlink, frame);
            ++s->frames_tx;

            av_log(priv, AV_LOG_TRACE, "%s: Filtered: %s\n", __func__, av_err2str(rv));
            did_something = 1;

            if (s->drain_state != DRAIN_NONE && pts_track_count(&s->track) == 0) {
                av_log(priv, AV_LOG_DEBUG, "%s: --- DQ last - drain done\n", __func__);
                s->drain_state = DRAIN_DONE;
            }
        }

        cn = count_enqueued(&s->capture);
    }

    if (s->drain_state == DRAIN_DONE) {
        ff_outlink_set_status(outlink, s->drain, s->drain_pts);
        av_log(priv, AV_LOG_TRACE, ">>> %s: Status done: %s\n", __func__, av_err2str(s->drain));
        return 0;
    }

    recycle_q(&s->output);
    n = count_enqueued(&s->output);

    while (n < 6 && !s->drain) {
        AVFrame * frame;
        int rv;

        if ((rv = ff_inlink_consume_frame(inlink, &frame)) < 0) {
            av_log(priv, AV_LOG_ERROR, "%s: consume in failed: %s\n", __func__, av_err2str(rv));
            return rv;
        }

        if (frame == NULL) {
            av_log(priv, AV_LOG_TRACE, "%s: No frame\n", __func__);
            if (!ack_inlink(avctx, s, inlink)) {
                ff_inlink_request_frame(inlink);
                av_log(priv, AV_LOG_TRACE, "%s: req frame\n", __func__);
            }
            break;
        }
        ++s->frames_rx;

        rv = deint_v4l2m2m_filter_frame(inlink, frame);
        av_frame_free(&frame);

        if (rv != 0)
            return rv;

        av_log(priv, AV_LOG_TRACE, "%s: Q frame\n", __func__);
        did_something = 1;
        ++n;
    }

    if ((n > 4 || s->drain) && ff_outlink_frame_wanted(outlink)) {
        ff_filter_set_ready(avctx, 1);
        did_something = 1;
        av_log(priv, AV_LOG_TRACE, "%s: ready\n", __func__);
    }

    av_log(priv, AV_LOG_TRACE, ">>> %s: OK (n=%d, cn=%d)\n", __func__, n, cn);
    return did_something ? 0 : FFERROR_NOT_READY;
}

static av_cold int common_v4l2m2m_init(AVFilterContext * const avctx, const filter_type_v4l2_t filter_type)
{
    DeintV4L2M2MContext * const priv = avctx->priv;
    DeintV4L2M2MContextShared * const ctx = av_mallocz(sizeof(DeintV4L2M2MContextShared));

    if (!ctx) {
        av_log(priv, AV_LOG_ERROR, "%s: error %d allocating context\n", __func__, 0);
        return AVERROR(ENOMEM);
    }
    priv->shared = ctx;
    ctx->logctx = priv;
    ctx->filter_type = filter_type;
    ctx->fd = -1;
    ctx->output.ctx = ctx;
    ctx->output.num_buffers = 8;
    ctx->output.name = "OUTPUT";
    ctx->capture.ctx = ctx;
    ctx->capture.num_buffers = 12;
    ctx->capture.name = "CAPTURE";
    ctx->done = 0;
    ctx->field_order = V4L2_FIELD_ANY;

    pts_track_init(&ctx->track, priv);

    atomic_init(&ctx->refcount, 1);

    if (priv->output_format_string) {
        ctx->output_format = av_get_pix_fmt(priv->output_format_string);
        if (ctx->output_format == AV_PIX_FMT_NONE) {
            av_log(avctx, AV_LOG_ERROR, "Invalid ffmpeg output format '%s'.\n", priv->output_format_string);
            return AVERROR(EINVAL);
        }
        if (fmt_av_to_v4l2(ctx->output_format) == 0) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported output format for V4L2: %s.\n", av_get_pix_fmt_name(ctx->output_format));
            return AVERROR(EINVAL);
        }
    } else {
        // Use the input format once that is configured.
        ctx->output_format = AV_PIX_FMT_NONE;
    }

#define STRING_OPTION(var_name, func_name, default_value) do { \
        if (priv->var_name ## _string) { \
            int var = av_ ## func_name ## _from_name(priv->var_name ## _string); \
            if (var < 0) { \
                av_log(avctx, AV_LOG_ERROR, "Invalid %s.\n", #var_name); \
                return AVERROR(EINVAL); \
            } \
            priv->var_name = var; \
        } else { \
            priv->var_name = default_value; \
        } \
    } while (0)

    STRING_OPTION(colour_primaries, color_primaries, AVCOL_PRI_UNSPECIFIED);
    STRING_OPTION(colour_transfer,  color_transfer,  AVCOL_TRC_UNSPECIFIED);
    STRING_OPTION(colour_matrix,    color_space,     AVCOL_SPC_UNSPECIFIED);
    STRING_OPTION(chroma_location,  chroma_location, AVCHROMA_LOC_UNSPECIFIED);

    return 0;
}

static av_cold int deint_v4l2m2m_init(AVFilterContext *avctx)
{
    return common_v4l2m2m_init(avctx, FILTER_V4L2_DEINTERLACE);
}

static av_cold int scale_v4l2m2m_init(AVFilterContext *avctx)
{
    int rv;
    DeintV4L2M2MContext * priv;
    DeintV4L2M2MContextShared * ctx;

    if ((rv = common_v4l2m2m_init(avctx, FILTER_V4L2_SCALE)) != 0)
        return rv;

    priv = avctx->priv;
    ctx = priv->shared;

    ctx->one_to_one = 1;
    return 0;
}

static void deint_v4l2m2m_uninit(AVFilterContext *avctx)
{
    DeintV4L2M2MContext *priv = avctx->priv;
    DeintV4L2M2MContextShared *ctx = priv->shared;

    av_log(priv, AV_LOG_VERBOSE, "Frames Rx: %u, Frames Tx: %u\n",
           ctx->frames_rx, ctx->frames_tx);
    ctx->done = 1;
    ctx->logctx = NULL;  // Log to NULL works, log to missing crashes
    pts_track_uninit(&ctx->track);
    deint_v4l2m2m_destroy_context(ctx);
}

static const AVOption deinterlace_v4l2m2m_options[] = {
    { NULL },
};

AVFILTER_DEFINE_CLASS(deinterlace_v4l2m2m);

#define OFFSET(x) offsetof(DeintV4L2M2MContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption scale_v4l2m2m_options[] = {
    { "w", "Output video width",
      OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = FLAGS },
    { "h", "Output video height",
      OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = FLAGS },
    { "format", "Output video format (software format of hardware frames)",
      OFFSET(output_format_string), AV_OPT_TYPE_STRING, .flags = FLAGS },
      // These colour properties match the ones of the same name in vf_scale.
      { "out_color_matrix", "Output colour matrix coefficient set",
      OFFSET(colour_matrix_string), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    { "out_range", "Output colour range",
      OFFSET(colour_range), AV_OPT_TYPE_INT, { .i64 = AVCOL_RANGE_UNSPECIFIED },
      AVCOL_RANGE_UNSPECIFIED, AVCOL_RANGE_JPEG, FLAGS, "range" },
        { "full",    "Full range",
          0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_JPEG }, 0, 0, FLAGS, "range" },
        { "limited", "Limited range",
          0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_MPEG }, 0, 0, FLAGS, "range" },
        { "jpeg",    "Full range",
          0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_JPEG }, 0, 0, FLAGS, "range" },
        { "mpeg",    "Limited range",
          0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_MPEG }, 0, 0, FLAGS, "range" },
        { "tv",      "Limited range",
          0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_MPEG }, 0, 0, FLAGS, "range" },
        { "pc",      "Full range",
          0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_JPEG }, 0, 0, FLAGS, "range" },
    // These colour properties match the ones in the VAAPI scaler
    { "out_color_primaries", "Output colour primaries",
      OFFSET(colour_primaries_string), AV_OPT_TYPE_STRING,
      { .str = NULL }, .flags = FLAGS },
    { "out_color_transfer", "Output colour transfer characteristics",
      OFFSET(colour_transfer_string),  AV_OPT_TYPE_STRING,
      { .str = NULL }, .flags = FLAGS },
    { "out_chroma_location", "Output chroma sample location",
      OFFSET(chroma_location_string),  AV_OPT_TYPE_STRING,
      { .str = NULL }, .flags = FLAGS },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 2, FLAGS, "force_oar" },
    { "force_divisible_by", "enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 1}, 1, 256, FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(scale_v4l2m2m);

static const AVFilterPad deint_v4l2m2m_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad deint_v4l2m2m_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = deint_v4l2m2m_config_props,
    },
};

AVFilter ff_vf_deinterlace_v4l2m2m = {
    .name           = "deinterlace_v4l2m2m",
    .description    = NULL_IF_CONFIG_SMALL("V4L2 M2M deinterlacer"),
    .priv_size      = sizeof(DeintV4L2M2MContext),
    .init           = &deint_v4l2m2m_init,
    .uninit         = &deint_v4l2m2m_uninit,
    FILTER_INPUTS(deint_v4l2m2m_inputs),
    FILTER_OUTPUTS(deint_v4l2m2m_outputs),
    FILTER_SINGLE_SAMPLEFMT(AV_PIX_FMT_DRM_PRIME),
    .priv_class     = &deinterlace_v4l2m2m_class,
    .activate       = deint_v4l2m2m_activate,
};

AVFilter ff_vf_scale_v4l2m2m = {
    .name           = "scale_v4l2m2m",
    .description    = NULL_IF_CONFIG_SMALL("V4L2 M2M scaler"),
    .priv_size      = sizeof(DeintV4L2M2MContext),
    .init           = &scale_v4l2m2m_init,
    .uninit         = &deint_v4l2m2m_uninit,
    FILTER_INPUTS(deint_v4l2m2m_inputs),
    FILTER_OUTPUTS(deint_v4l2m2m_outputs),
    FILTER_SINGLE_SAMPLEFMT(AV_PIX_FMT_DRM_PRIME),
    .priv_class     = &scale_v4l2m2m_class,
    .activate       = deint_v4l2m2m_activate,
};

