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
 * GIF demuxer.
 */

#include "avformat.h"
#include "libavutil/bprint.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "internal.h"
#include "libavcodec/flif16.h"

#include "config.h"
#if CONFIG_ZLIB
#include <zlib.h>
#endif


static int flif16_probe(const AVProbeData *p)
{
    uint32_t vlist[3] = {0};
    unsigned int count = 0, pos = 0;

    // Magic Number
    if (memcmp(p->buf, flif16_reader, 4)) {
        return 0;
    }

    for(int i = 0; i < 2 + (((p->buf[4] >> 4) > 4) ? 1 : 0); ++i) {
        while (p->buf[5 + pos] > 127) {
            if (!(count--)) {
                return 0;
            }
            VARINT_APPEND(*vlist[i], p->buf[5 + pos]);
            ++pos;
        }
        VARINT_APPEND(*vlist[i], p->buf[5 + pos]);
        count = 0;
    }

    if (!((vlist[0] + 1) && (vlist[1] + 1)))
        return 0;

    if ((p->buf[4] >> 4) > 4) && !(vlist[2] + 2))
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int flif16_read_header(AVFormatContext *s)
{
    GIFDemuxContext *gdc = s->priv_data;
    AVIOContext     *pb  = s->pb;
    AVStream        *st;
    uint32_t vlist[3] = {0};
    uint32_t metadata_size = 0;
    uint8_t flag, temp;
    uint8_t tag[5] = {0};
    unsigned int count = 3;

    // Magic Number
    if (avio_rb32(&s->gb) != (*((uint32_t *) flif16_header))) {
        av_log(avctx, AV_LOG_ERROR, "bad magic number\n");
        return AVERROR(EINVAL);
    }
    
    st = avformat_new_stream(s, NULL);
    flag = avio_r8(pb) >> 4;

    for (int i = 0; i < 2 + ((flag > 4) ? 1 : 0); ++i) {
        while ((temp = avio_r8(pb)) > 127) {
            if (!(count--))
                return AVERROR_INVALIDDATA;
            VARINT_APPEND(*vlist[i], temp);
        }
        VARINT_APPEND(*vlist[i], temp);
        count = 3;
    }

    ++vlist[0];
    ++vlist[1];
    if ((p->buf[4] >> 4) > 4))
        vlist[2] += 2;

    while ((temp = avio_r8(pb)) != 0) {
        // Get metadata identifier
        tag[0] = temp;
        for(int i = 1; i <= 3; ++i)
            tag[i] = avio_r8(pb)

        // Read varint
        while ((temp = avio_r8(pb)) > 127) {
            if (!(count--))
                return AVERROR_INVALIDDATA;
            VARINT_APPEND(meta_size, temp);
        }
        FF_FLIF16_VARINT_APPEND(s->meta, temp);
        count = 3;

        #if CONFIG_ZLIB
            // Decompression Routines
        #endif
    }
    
    // The minimum possible delay in a FLIF16 image is 1 millisecond.
    // Therefore time base is 10^-3, i.e. 1/1000
    avpriv_set_pts_info(st, 64, 1, 1000);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_FLIF16;
    st->codecpar->width      = vlist[0];
    st->codecpar->height     = vlist[1];
    st->start_time           = 0;
    st->nb_frames            = vlist[2];

    // Jump to start because flif16 decoder needs header data too
    if (avio_seek(pb, 0, SEEK_SET) != 0)
        return AVERROR(EIO);

    return 0;
}

static const AVOption options[] = {
    { NULL }
};

static const AVClass demuxer_class = {
    .class_name = "FLIF16 demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

AVInputFormat ff_gif_demuxer = {
    .name           = "flif16",
    .long_name      = NULL_IF_CONFIG_SMALL("Free Lossless Image Format (FLIF)"),
    .priv_data_size = sizeof(GIFDemuxContext),
    .read_probe     = flif16_probe,
    .read_header    = flif16_read_header,
    .flags          = AVFMT_GENERIC_INDEX,
    .priv_class     = &demuxer_class,
};
