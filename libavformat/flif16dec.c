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


static int flif16_probe(const AVProbeData *p)
{
    // Magic Number
    if (memcmp(p->buf, flif16_reader, 4)) {
        av_log(avctx, AV_LOG_ERROR, "bad magic number\n");
        return 0;
    }

    /* width or height contains zero? */
    if (!AV_RL16(&p->buf[6]) || !AV_RL16(&p->buf[8]))
        return 0;
    // Width or height is zero?

    return AVPROBE_SCORE_MAX;
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
    .read_probe     = gif_probe,
    .read_header    = gif_read_header,
    .read_packet    = gif_read_packet,
    .flags          = AVFMT_GENERIC_INDEX,
    .priv_class     = &demuxer_class,
};
