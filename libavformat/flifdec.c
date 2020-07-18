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
//remove
#include <stdio.h>
#if CONFIG_ZLIB
#include <zlib.h>
#endif


static int flif16_probe(const AVProbeData *p)
{
    uint32_t vlist[3] = {0};
    unsigned int count = 0, pos = 0;

    printf("[%s] called\n", __func__);
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
    AVIOContext     *pb  = s->pb;
    AVStream        *st;
    uint32_t vlist[3] = {0};
    uint32_t metadata_size = 0;
    uint8_t flag, temp;
    uint8_t tag[5] = {0};
    unsigned int count = 4;

    printf("[%s] called\n", __func__);
    // Magic Number
    if (avio_rl32(pb) != (*((uint32_t *) flif16_header))) {
        av_log(s, AV_LOG_ERROR, "bad magic number\n");
        return AVERROR(EINVAL);
    }

    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);

    st = avformat_new_stream(s, NULL);
    flag = avio_r8(pb) >> 4;

    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);

    for (int i = 0; i < 2 + ((flag > 4) ? 1 : 0); ++i) {
        printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
        while ((temp = avio_r8(pb)) > 127) {
            if (!(count--))
                return AVERROR_INVALIDDATA;
            VARINT_APPEND(vlist[i], temp);
        }
        VARINT_APPEND(vlist[i], temp);
        count = 4;
    }

    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);

    ++vlist[0];
    ++vlist[1];
    if (flag > 4)
        vlist[2] += 2;

    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);

    while ((temp = avio_r8(pb)) != 0) {
        printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
        // Get metadata identifier
        tag[0] = temp;
        for(int i = 1; i <= 3; ++i)
            tag[i] = avio_r8(pb);
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
        // Read varint
        while ((temp = avio_r8(pb)) > 127) {
            if (!(count--))
                return AVERROR_INVALIDDATA;
            VARINT_APPEND(metadata_size, temp);
        }
        VARINT_APPEND(metadata_size, temp);
        count = 4;
        printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
        #if CONFIG_ZLIB
            // Decompression Routines
        #else
            avio_seek(pb, metadata_size, SEEK_CUR);
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
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    return 0;
}

static int flif16_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb  = s->pb;
    int ret;
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    printf("Returning %d\n", pkt->size);
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
    .priv_data_size = 0,
    .extensions     = "flif",
    .read_probe     = flif16_probe,
    .read_header    = flif16_read_header,
    .read_packet    = flif16_read_packet,
    .priv_class     = &demuxer_class,
};
