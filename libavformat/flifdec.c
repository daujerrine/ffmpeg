/*
 * FLIF16 demuxer
 * Copyright (c) 2020 Anamitra Ghorui <aghorui@teknik.io>
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
 * FLIF demuxer.
 */

#include "avformat.h"
#include "libavutil/common.h"
#include "libavutil/bprint.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "internal.h"
#include "libavcodec/exif.h"

#include "libavcodec/flif16.h"
#include "libavcodec/flif16_rangecoder.h"

#include "config.h"

#if CONFIG_ZLIB
#include <zlib.h>
#endif

/*
 * FLIF's reference encoder currently encodes metadata as a raw DEFLATE stream
 * (RFC 1951). In order to decode a raw deflate stream using Zlib, inflateInit2
 * must be used with windowBits being between -8 .. -15.
 */
#define ZLIB_WINDOW_BITS -15
#define BUF_SIZE 4096

typedef struct FLIFDemuxContext {
    const AVClass *class;
#if CONFIG_ZLIB
    z_stream stream;
    uint8_t active;
#endif
} FLIFDemuxContext;


#if CONFIG_ZLIB
static int flif_inflate(FLIFDemuxContext *s, uint8_t *buf, int buf_size,
                        uint8_t **out_buf, int *out_buf_size)
{
    int ret;
    z_stream *stream = &s->stream;

    if (!s->active) {
        s->active = 1;
        stream->zalloc   = Z_NULL;
        stream->zfree    = Z_NULL;
        stream->opaque   = Z_NULL;
        stream->avail_in = 0;
        stream->next_in  = Z_NULL;
        ret = inflateInit2(stream, ZLIB_WINDOW_BITS);

        if (ret != Z_OK)
            return ret;

        *out_buf_size = buf_size;
        *out_buf = av_realloc_f(*out_buf, *out_buf_size, 1);
        if (!*out_buf)
            return AVERROR(ENOMEM);
    }

    stream->next_in  = buf;
    stream->avail_in = buf_size;

    do {
        while (stream->total_out >= (*out_buf_size - 1)) {
            *out_buf = av_realloc_f(*out_buf, (*out_buf_size) * 2, 1);
            if (!*out_buf)
                return AVERROR(ENOMEM);
            *out_buf_size *= 2;
        }

        stream->next_out  = *out_buf + stream->total_out;
        stream->avail_out = *out_buf_size - stream->total_out - 1;
     
        ret = inflate(stream, Z_PARTIAL_FLUSH);

        switch (ret) {
        case Z_NEED_DICT:
        case Z_DATA_ERROR:
            (void)inflateEnd(stream);
            return AVERROR_INVALIDDATA;
        case Z_MEM_ERROR:
            (void)inflateEnd(stream);
            return AVERROR(ENOMEM);
        }
    } while (stream->avail_in > 0);

    if (ret == Z_STREAM_END) {
        s->active = 0;
        (*out_buf)[stream->total_out] = '\0';
        (void) inflateEnd(stream);
    } else
        ret = AVERROR(EAGAIN);

    return ret; // Return Z_BUF_ERROR/EAGAIN as long as input is incomplete.
}
#endif

#if CONFIG_EXIF
static int flif_read_exif(void *logctx, uint8_t *buf, int buf_size, AVDictionary **d)
{
    uint8_t le;
    uint32_t temp;
    int ret;
    GetByteContext gb;
    // Read exif header
    if (memcmp("Exif", buf, 4))
        return AVERROR_INVALIDDATA;

    buf += 6;

    // Figure out endianness
    if (buf[0] == 'M' && buf[1] == 'M')
        le = 0;
    else if (buf[0] == 'I' && buf[1] == 'I')
        le = 1;
    else
        return AVERROR_INVALIDDATA;

    buf += 2;

    bytestream2_init(&gb, buf, buf_size - 8);
    temp = ff_tget_short(&gb, le);

    // Check TIFF marker
    if (temp != 0x002A)
        return AVERROR_INVALIDDATA;

    buf += 2;

    if (le)
        temp = bytestream2_get_le32(&gb);
    else
        temp = bytestream2_get_be32(&gb);

    // Subtract read bytes, then skip
    bytestream2_skip(&gb, temp - 8);

    ret = ff_exif_decode_ifd(logctx, &gb, le, 0, d);
    
    return ret;
}
#endif

static int flif16_probe(const AVProbeData *p)
{
    uint32_t vlist[3] = {0};
    unsigned int count = 0, pos = 0;

    // Magic Number
    if (memcmp(p->buf, flif16_header, 4)) {
        return 0;
    }

    for(int i = 0; i < 2 + (((p->buf[4] >> 4) > 4) ? 1 : 0); i++) {
        while (p->buf[5 + pos] > 127) {
            if (!(count--)) {
                return 0;
            }
            VARINT_APPEND(vlist[i], p->buf[5 + pos]);
            ++pos;
        }
        VARINT_APPEND(vlist[i], p->buf[5 + pos]);
        count = 0;
    }

    if (!((vlist[0] + 1) && (vlist[1] + 1)))
        return 0;

    if (((p->buf[4] >> 4) > 4) && !(vlist[2] + 2))
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int flif16_read_header(AVFormatContext *s)
{
    FLIFDemuxContext *dc = s->priv_data;
    GetByteContext gb;
    FLIF16RangeCoder rc = (FLIF16RangeCoder) {0};

    AVIOContext     *pb  = s->pb;
    AVStream        *st;

    int64_t duration = 0;
    uint32_t vlist[3] = {0};
    uint32_t flag, animated, temp;
    uint32_t bpc = 0;
    uint32_t metadata_size = 0;
    int out_buf_size = 0;
    int buf_size = 0;
    unsigned int count = 4;
    int ret;
    int format;
    int segment = 0, i = 0;
    uint8_t tag[5] = {0};
    uint8_t buf[BUF_SIZE];
    uint8_t *out_buf = NULL;
    uint8_t loops = 0;
    uint8_t num_planes;
    uint8_t num_frames;

#if !CONFIG_ZLIB
    av_log(s, AV_LOG_WARNING, "ffmpeg has not been compiled with Zlib. Metadata may not be decoded.\n");
#endif

    // Magic Number
    if (avio_rl32(pb) != (*((uint32_t *) flif16_header))) {
        av_log(s, AV_LOG_ERROR, "bad magic number\n");
        return AVERROR_INVALIDDATA;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    flag = avio_r8(pb);
    animated = (flag >> 4) > 4;
    duration = !animated;
    bpc = avio_r8(pb); // Bytes per channel

    num_planes = flag & 0x0F;

    for (int i = 0; i < (2 + animated); i++) {
        while ((temp = avio_r8(pb)) > 127) {
            if (!(count--))
                return AVERROR_INVALIDDATA;
            VARINT_APPEND(vlist[i], temp);
        }
        VARINT_APPEND(vlist[i], temp);
        count = 4;
    }

    vlist[0]++;
    vlist[1]++;
    if (animated)
        vlist[2] += 2;
    else
        vlist[2] = 1;

    num_frames = vlist[2];

    while ((temp = avio_r8(pb))) {
        // Get metadata identifier
        tag[0] = temp;
        for(int i = 1; i <= 3; i++)
            tag[i] = avio_r8(pb);

        // Read varint
        while ((temp = avio_r8(pb)) > 127) {
            if (!(count--))
                return AVERROR_INVALIDDATA;
            VARINT_APPEND(metadata_size, temp);
        }
        VARINT_APPEND(metadata_size, temp);
        count = 4;

#if CONFIG_ZLIB
        /*
         * Decompression Routines
         * There are 3 supported metadata chunks currently in FLIF: eXmp, eXif,
         * and iCCp.
         */
        while (metadata_size > 0) {
            if ((buf_size = avio_read_partial(pb, buf, FFMIN(BUF_SIZE, metadata_size))) < 0)
                return buf_size;
            metadata_size -= buf_size;
            if((ret = flif_inflate(dc, buf, buf_size, &out_buf, &out_buf_size)) < 0 &&
                ret != AVERROR(EAGAIN)) {
                if (ret == AVERROR(ENOMEM) || ret == AVERROR_INVALIDDATA)
                    return ret;
                av_log(s, AV_LOG_ERROR, "could not decode metadata segment: %s\n", tag);
                avio_skip(pb, metadata_size);
                goto metadata_fail;
            }
        }

        switch (*((uint32_t *) tag)) {
        case MKTAG('e', 'X', 'i','f'):
#if CONFIG_EXIF
            ret = flif_read_exif(s, out_buf, out_buf_size, &s->metadata);
            if (ret < 0)
                av_log(s, AV_LOG_WARNING, "metadata may be corrupted\n");
#endif
            break;

        case MKTAG('i','C','C','P'):
            break;

        default:
            av_dict_set(&s->metadata, tag, out_buf, 0);
            break;
        }
#else
        avio_skip(pb, metadata_size);
#endif

        metadata_fail:
        continue;
    }

    av_freep(&out_buf);

    avio_read(pb, buf, FLIF16_RAC_MAX_RANGE_BYTES);
    ff_flif16_rac_init(&rc, NULL, buf, FLIF16_RAC_MAX_RANGE_BYTES);
    ret = avio_read_partial(pb, buf, BUF_SIZE);
    bytestream2_init(&gb, buf, ret);
    rc.gb = &gb;

    while (1) {
        switch (segment) {
        case 0:
            if (bpc == '0') {
                bpc = 0;
                for (; i < num_planes; i++) {
                    RAC_GET(&rc, NULL, 1, 15, &temp, FLIF16_RAC_UNI_INT8);
                    bpc = FFMAX(bpc, (1 << temp) - 1);
                }
                i = 0;
            } else
                bpc = (bpc == '1') ? 255 : 65535;
            if (num_frames < 2)
                goto end;
            segment++;

        case 1:
            if (num_planes > 3) {
                RAC_GET(&rc, NULL, 0, 1, &temp, FLIF16_RAC_UNI_INT8);
            }
            segment++;

        case 2:
            if (num_frames > 1) {
                RAC_GET(&rc, NULL, 0, 100, &loops, FLIF16_RAC_UNI_INT8);
            }
            if (!loops)
                loops = 1;
            segment++;

        case 3:
            if (num_frames > 1) {
                for (; i < num_frames; i++) {
                    temp = 0;
                    RAC_GET(&rc, NULL, 0, 60000, &(temp), FLIF16_RAC_UNI_INT16);
                    duration += temp;
                }
                i = 0;
            } else
                duration = 1;
            goto end;
        }

        need_more_data:
        if ((ret = avio_read_partial(pb, buf, BUF_SIZE)) < 0)
            return ret;
        bytestream2_init(&gb, buf, ret);
    }

    end:
    if (bpc > 65535) {
        av_log(s, AV_LOG_ERROR, "depth per channel greater than 16 bits not supported\n");
        return AVERROR_PATCHWELCOME;
    }

    format = flif16_out_frame_type[FFMIN(num_planes, 4)][bpc > 255];

    // The minimum possible delay in a FLIF16 image is 1 millisecond.
    // Therefore time base is 10^-3, i.e. 1/1000
    avpriv_set_pts_info(st, 64, 1, 1000);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_FLIF16;
    st->codecpar->width      = vlist[0];
    st->codecpar->height     = vlist[1];
    st->codecpar->format     = format;
    st->duration             = duration * loops;
    st->start_time           = 0;
    st->nb_frames            = vlist[2];
    st->need_parsing         = 1;

    // Jump to start because flif16 decoder needs header data too
    if (avio_seek(pb, 0, SEEK_SET) != 0)
        return AVERROR(EIO);
    return 0;
}


static int flif16_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb  = s->pb;
    int ret;
    ret = av_get_packet(pb, pkt, avio_size(pb));
    return ret;
}


static const AVOption options[] = {
    { NULL }
};

static const AVClass demuxer_class = {
    .class_name = "FLIF demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

AVInputFormat ff_flif_demuxer = {
    .name           = "flif",
    .long_name      = NULL_IF_CONFIG_SMALL("Free Lossless Image Format (FLIF)"),
    .priv_data_size = sizeof(FLIFDemuxContext),
    .extensions     = "flif",
    .read_probe     = flif16_probe,
    .read_header    = flif16_read_header,
    .read_packet    = flif16_read_packet,
    .priv_class     = &demuxer_class,
};
