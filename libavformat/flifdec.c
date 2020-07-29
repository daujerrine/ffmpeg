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
#include "libavutil/bprint.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "internal.h"
#include "libavcodec/flif16.h"
#include "libavcodec/flif16_rangecoder.h"

#include "config.h"
//remove
#if CONFIG_ZLIB
#include <zlib.h>
#endif

#define BUF_SIZE 4096

typedef struct FLIFDemuxContext {
    const AVClass *class;
#if 0
// CONFIG_ZLIB
    z_stream stream;
    uint8_t active;
#endif
    int64_t duration;
} FLIFDemuxContext;


#if 0
// CONFIG_ZLIB
static int flif_inflate(FLIFDemuxContext *s, unsigned char *buf, int buf_size,
                        unsigned char *out_buf, int *out_buf_size)
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
        ret   = inflateInit(stream);
        
        if (ret != Z_OK)
        return ret;

        *out_buf_size = buf_size;
        out_buf = av_realloc(out_buf, buf_size);
        if (!out_buf)
            return AVERROR(ENOMEM);
    }

    stream->next_in  = buf;
    stream->avail_in = buf_size;
    if(stream->total_out >= *out_buf_size) {
        out_buf = av_realloc(out_buf, (*out_buf_size) * 2);
        if (!out_buf)
            return AVERROR(ENOMEM);
        *out_buf_size *= 2;
    }

    stream->next_out  = out_buf + stream->total_out;
    stream->avail_out = *out_buf_size - stream->total_out - 1; // Last byte should be NULL char

    ret = inflate(stream, Z_PARTIAL_FLUSH);

    switch (ret) {
        case Z_NEED_DICT:
        case Z_DATA_ERROR:
            (void)inflateEnd(stream);
            return AVERROR(EINVAL);
        case Z_MEM_ERROR:
            (void)inflateEnd(stream);
            return AVERROR(ENOMEM);
    }

    if (ret == Z_STREAM_END) {
        ret = 0;
        s->active = 0;
        out_buf[stream->total_out - 1] = '\0';
        (void) inflateEnd(stream);
    } else
        ret = AVERROR(EAGAIN);

    return ret; // Return Z_BUF_ERROR/EAGAN as long as input is incomplete.
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

    for(int i = 0; i < 2 + (((p->buf[4] >> 4) > 4) ? 1 : 0); ++i) {
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
    // FLIFDemuxContext *dc = s->priv_data;
    GetByteContext gb;
    FLIF16RangeCoder rc;

    AVIOContext     *pb  = s->pb;
    AVStream        *st;
    
    uint32_t vlist[3] = {0};
    uint32_t flag, animated, temp;
    uint32_t bpc = 0;
    // uint8_t tag[5] = {0};
    uint8_t buf[BUF_SIZE];
    uint32_t metadata_size = 0;
    // uint8_t *out_buf = NULL;
    // int out_buf_size = 0;

    unsigned int count = 4;
    int ret;
    int format;
    int segment = 0, i = 0;
    int64_t duration = 0;
    uint8_t loops = 0;
    uint8_t num_planes;
    uint8_t num_frames;

    // Magic Number
    if (avio_rl32(pb) != (*((uint32_t *) flif16_header))) {
        av_log(s, AV_LOG_ERROR, "bad magic number\n");
        return AVERROR(EINVAL);
    }

    st = avformat_new_stream(s, NULL);
    flag = avio_r8(pb);
    animated = (flag >> 4) > 4;
    duration = !animated;
    bpc = avio_r8(pb); // Bytes per channel

    num_planes = flag & 0x0F;

    for (int i = 0; i < (2 + animated); ++i) {
        while ((temp = avio_r8(pb)) > 127) {
            if (!(count--))
                return AVERROR_INVALIDDATA;
            VARINT_APPEND(vlist[i], temp);
        }
        VARINT_APPEND(vlist[i], temp);
        count = 4;
    }


    ++vlist[0];
    ++vlist[1];
    if (animated)
        vlist[2] += 2;
    else
        vlist[2] = 1;

    num_frames = vlist[2];

    while ((temp = avio_r8(pb))) {
        // Get metadata identifier
        #if 0
        tag[0] = temp;
        for(int i = 1; i <= 3; ++i)
            tag[i] = avio_r8(pb);
        #endif
        
        avio_seek(pb, 3, SEEK_CUR);

        // Read varint
        while ((temp = avio_r8(pb)) > 127) {
            if (!(count--))
                return AVERROR_INVALIDDATA;
            VARINT_APPEND(metadata_size, temp);
        }
        VARINT_APPEND(metadata_size, temp);
        count = 4;
        
        #if 0
        // CONFIG_ZLIB
        // TODO see why this does not work.
        // Decompression Routines
        while (metadata_size > 0) {
            ret = avio_read(pb, metadata_buf, FFMIN(METADATA_BUF_SIZE, metadata_size));
            metadata_size -= ret;
            if((ret = flif_inflate(dc, metadata_buf, ret, out_buf, &out_buf_size)) < 0 &&
                ret != AVERROR(EAGAIN)) {
                av_log(s, AV_LOG_ERROR, "could not decode metadata\n");
                return ret;
            }
        }
        av_dict_set(&s->metadata, tag, out_buf, 0);
        #else
        avio_seek(pb, metadata_size, SEEK_CUR);
        #endif
    }

    #if 0
    // CONFIG_ZLIB
    if (out_buf)
        av_freep(&out_buf);
    #endif

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
                    for (; i < num_planes; ++i) {
                        RAC_GET(&rc, NULL, 1, 15, &temp, FLIF16_RAC_UNI_INT8);
                        bpc = FFMAX(bpc, (1 << temp) - 1);
                    }
                    i = 0;
                } else
                    bpc = (bpc == '1') ? 255 : 65535;
                // MSG("planes : %d & bpc : %d\n", num_planes, bpc);
                if (num_frames < 2)
                    goto end;
                ++segment;

            case 1:
                if (num_planes > 3) {
                    RAC_GET(&rc, NULL, 0, 1, &temp, FLIF16_RAC_UNI_INT8);
                }
                ++segment;

            case 2:
                if (num_frames > 1) {
                    RAC_GET(&rc, NULL, 0, 100, &loops, FLIF16_RAC_UNI_INT8);
                }
                loops = (!loops) ? 1 : loops;
                ++segment;

            case 3:
                if (num_frames > 1) {
                    for (; i < num_frames; ++i) {
                        temp = 0;
                        RAC_GET(&rc, NULL, 0, 60000, &(temp), FLIF16_RAC_UNI_INT16);
                        duration += temp;
                    }
                    i = 0;
                }
                goto end;
        }

        need_more_data:
            avio_read_partial(pb, buf, BUF_SIZE);
            bytestream2_init(&gb, buf, BUF_SIZE);
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
    // st->need_parsing         = 1;

    // Jump to start because flif16 decoder needs header data too
    if (avio_seek(pb, 0, SEEK_SET) != 0)
        return AVERROR(EIO);
    return 0;
}


static int flif16_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb  = s->pb;
    int ret;
    //  FFMIN(BUF_SIZE, avio_size(pb))
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
    //.flags          = AVFMT_NOTIMESTAMPS,
    .priv_class     = &demuxer_class,
};
