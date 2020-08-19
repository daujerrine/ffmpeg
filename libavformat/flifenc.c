/*
 * FLIF muxer
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

#include "avformat.h"
#include "internal.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavcodec/bytestream.h"
#include "libavcodec/flif16.h"

#define PRINT_LINE printf("At: %s, %s %d\n", __func__, __FILE__, __LINE__);


static int flif16_write_header(AVFormatContext *s)
{
    if (s->nb_streams != 1 ||
        s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        s->streams[0]->codecpar->codec_id   != AV_CODEC_ID_FLIF16) {
        av_log(s, AV_LOG_ERROR,
               "incorrect stream configuration for FLIF muxer.\n");
        PRINT_LINE
        return AVERROR(EINVAL);
    }
    PRINT_LINE
    avpriv_set_pts_info(s->streams[0], 64, 1, 1000);

    return 0;
}

static int flif16_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;
    printf("Writing %d\n", pkt->size);
    avio_write(pb, pkt->data, pkt->size);
    PRINT_LINE
    return 0;
}

AVOutputFormat ff_flif_muxer = {
    .name           = "flif",
    .long_name      = NULL_IF_CONFIG_SMALL("Free Lossless Image Format (FLIF)"),
    .extensions     = "flif",
    .audio_codec    = AV_CODEC_ID_NONE,
    .video_codec    = AV_CODEC_ID_FLIF16,
    .write_header   = flif16_write_header,
    .write_packet   = flif16_write_packet,
    .flags          = AVFMT_VARIABLE_FPS,
};
