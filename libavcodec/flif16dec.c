/*
 * FLIF16 Decoder
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
 * FLIF16 Decoder
*/

#include <stdio.h> // Remove

#include "flif16.h"
#include "flif16_rangecoder.h"

#include "avcodec.h"
#include "libavutil/common.h"
#include "bytestream.h"

/*
 * Due to the nature of the format, the decoder has to take the entirety of the
 * data before it can generate any frames. The decoder has to return
 * AVERROR(EAGAIN) as long as the bitstream is incomplete.
 */

// TODO prefix approprate functions with ff_*

enum FLIF16States {
    FLIF16_HEADER = 1,
    FLIF16_SECONDHEADER,
    FLIF16_TRANSFORM,
    FLIF16_MANIAC,
    FLIF16_PIXELDATA,
    FLIF16_CHECKSUM
};

static int ff_flif16_read_header(AVCodecContext *avctx)
{
    uint8_t temp, count = 3;
    FLIF16DecoderContext *s = avctx->priv_data;
    // TODO Make do without this array
    uint32_t *vlist[] = { &s->width, &s->height, &s->frames };
    // Minimum size has empirically found to be 8 bytes.

    if (bytestream2_size(&s->gb) < 8) {
        av_log(avctx, AV_LOG_ERROR, "buf size too small (%d)\n",
               bytestream2_size(&s->gb));
        return AVERROR(EINVAL);
    }

    if (bytestream2_get_le32(&s->gb) != (*((uint32_t *) flif16_header))) {
        av_log(avctx, AV_LOG_ERROR, "bad magic number\n");
        return AVERROR(EINVAL);
    }

    s->state = FLIF16_HEADER;

    temp = bytestream2_get_byte(&s->gb);
    s->ia       = temp >> 4;
    s->channels = (0x0F & temp);
    s->bpc      = bytestream2_get_byte(&s->gb);

    // Handle dimensions and frames
    for(int i = 0; i < 2 + ((s->ia > 4) ? 1 : 0); ++i) {
        while ((temp = bytestream2_get_byte(&s->gb)) > 127) {
            FF_FLIF16_VARINT_APPEND(*vlist[i], temp);
            if (!count) {
                av_log(avctx, AV_LOG_ERROR, "image dimensions too big\n");
                return AVERROR(ENOMEM);
            }
        }
        FF_FLIF16_VARINT_APPEND(*vlist[i], temp);
        count = 3;
    }
    __PLN__
    s->width++;
    s->height++;
    (s->ia > 4) ? (s->frames += 2) : (s->frames = 1);

    // Handle Metadata Chunk. Currently it discards all data.
    __PLN__
    while ((temp = bytestream2_get_byte(&s->gb)) != 0) {
        bytestream2_seek(&s->gb, 3, SEEK_CUR);
        // Read varint
        while ((temp = bytestream2_get_byte(&s->gb)) > 127) {
            FF_FLIF16_VARINT_APPEND(s->meta, temp);
            if (!count) {
                av_log(avctx, AV_LOG_ERROR, "metadata chunk too big \n");
                return AVERROR(ENOMEM);
            }
        }
        FF_FLIF16_VARINT_APPEND(s->meta, temp);
        bytestream2_seek(&s->gb, s->meta, SEEK_CUR);
    }

    printf("[%s] left = %d\n", __func__, bytestream2_get_bytes_left(&s->gb));
    s->state = FLIF16_SECONDHEADER;
    return 0;
}

static int ff_flif16_read_second_header(AVCodecContext *avctx)
{
    uint32_t temp;
    FLIF16DecoderContext *s = avctx->priv_data;

    if (!s->rc) {
        s->buf_count += bytestream2_get_buffer(&s->gb, s->buf,
                                               FFMIN(bytestream2_get_bytes_left(&s->gb),
                                               (FLIF16_RAC_MAX_RANGE_BYTES - s->buf_count)));
        if (s->buf_count < FLIF16_RAC_MAX_RANGE_BYTES)
            return AVERROR(EAGAIN);

        s->rc = ff_flif16_rac_init(&s->gb, s->buf, s->buf_count);
    }

    switch (s->segment) {
        case 0:
            // In original source this is handled in what seems to be a very
            // bogus manner. It takes all the bpps of all channels and then
            // takes the max.
            if (s->bpc == '0') {
                s->bpc = 0;
                for (; s->i < s->channels; ++s->i) {
                    RAC_GET(s->rc, NULL, 1, 15, &temp, FLIF16_RAC_UNI_INT);
                    s->bpc = FFMAX(s->bpc, (1 << temp) - 1);
                }
            } else
                s->bpc = (s->bpc == '1') ? 255 : 65535;
            s->i = 0;
            s->range = ff_flif16_ranges_static_init(s->channels, s->bpc);
            printf("channels : %d & bpc : %d\n", s->channels, s->bpc);
            /*
            s->ranges = av_malloc(s->channels * sizeof(*(s->ranges)));
            for (int i = 0; i < s->channels; ++i)
                RANGE_SET(s->ranges[i], 0, s->bpc);
            //for(int i = 0; i < s->channels; ++i)
            //    s->src_ranges->max[i] = s->bpc;
            ++s->segment; __PLN__*/

        case 1:
            if (s->channels > 3)
                RAC_GET(s->rc, NULL, 0, 1, (uint32_t *) &s->alphazero,
                        FLIF16_RAC_UNI_INT);
            ++s->segment; __PLN__

        case 2:
            if (s->frames > 1) {
                RAC_GET(s->rc, NULL, 0, 100, (uint32_t *) &s->loops,
                        FLIF16_RAC_UNI_INT);
                s->framedelay = av_mallocz(sizeof(*(s->framedelay)) * s->frames);
            }
            ++s->segment; __PLN__

        case 3:
            if (s->frames > 1) {
                for (; (s->i) < (s->frames); ++(s->i)) {
                    RAC_GET(s->rc, NULL, 0, 60000, &(s->framedelay[(s->i)]),
                            FLIF16_RAC_UNI_INT);
                }
                s->i = 0;
            }
            ++s->segment;

        case 4:
            // Has custom alpha flag
            RAC_GET(s->rc, NULL, 0, 1, &temp, FLIF16_RAC_UNI_INT);
            printf("[%s] has_custom_cutoff_alpha = %d\n", __func__, temp);
            ++s->segment;

        case 5:
            if (temp)
                RAC_GET(s->rc, NULL, 1, 128, &s->cut, FLIF16_RAC_UNI_INT);
            ++s->segment;

        case 6:
            if (temp)
                RAC_GET(s->rc, NULL, 2, 128, &s->alpha, FLIF16_RAC_UNI_INT);
            ++s->segment;

        case 7:
            if (temp)
                RAC_GET(s->rc, NULL, 0, 1, &s->custombc, FLIF16_RAC_UNI_INT);
            if (s->custombc) {
                av_log(avctx, AV_LOG_ERROR,
                       "custom bitchances not implemented\n");
                return AVERROR_PATCHWELCOME;
            }
            goto end;
    }

    end:
    s->state   = FLIF16_TRANSFORM;
    s->segment = 0;

    #ifdef MULTISCALE_CHANCES_ENABLED
    s->rc->mct = ff_flif16_multiscale_chancetable_init();
    ff_flif16_build_log4k_table(&s->rc->log4k);
    #endif
    
    ff_flif16_chancetable_init(&s->rc->ct,
                               CHANCETABLE_DEFAULT_ALPHA,
                               CHANCETABLE_DEFAULT_CUT);
    // return AVERROR_EOF; // Remove this when testing out transforms.
    return 0;

    need_more_data:
    printf("[%s] Need more data\n", __func__);
    return AVERROR(EAGAIN);
}


static int ff_flif16_read_transforms(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    uint8_t temp;
    loop:
    switch (s->segment) {
        case 0:
            RAC_GET(s->rc, NULL, 0, 0, &temp, FLIF16_RAC_BIT);
            if(!temp)
                goto end;
            //av_log(avctx, AV_LOG_ERROR, "transforms not implemented\n");
            //return AVERROR_PATCHWELCOME;
            ++s->segment;

        case 1:
            RAC_GET(s->rc, NULL, 0, 13, &temp, FLIF16_RAC_UNI_INT);
            printf("Transform : %d\n", temp);
            //s->prev_range = ff_flif16_ranges_static_init(s->channels, s->bpc); 
            s->transforms[s->transform_top] = ff_flif16_transform_init(temp, s->range, s->transforms[s->transform_top]);
            //printf("%d\n", s->transforms[s->transform_top]->t_no);
            ff_flif16_transform_read(s->transforms[s->transform_top], s,
                                     s->range);
            s->range = ff_flif16_transform_meta(s->transforms[s->transform_top],
                                                s->range);
            printf("Ranges : %d\n", s->range->r_no);
            s->segment = 0;
            s->transform_top++;
            goto loop;

        case 2:
            end:
            s->segment = 2;
            // Read invisible pixel predictor
            /*
             * if (alphazero && ranges->numPlanes() > 3 
             *     && ranges->min(3) <= 0 
             *     && encoding == flifEncoding::interlaced) {
             */
            if ( s->alphazero && s->channels > 3
                && ff_flif16_ranges_min(s->range, 3) <= 0
                && !(s->ia % 2))
                RAC_GET(s->rc, NULL, 0, 2, &s->ipp, FLIF16_RAC_UNI_INT);
    }

    s->state  = FLIF16_MANIAC;
    return 0;

    need_more_data:
    return AVERROR(EAGAIN);
}

static int ff_flif16_read_maniac_forest(AVCodecContext *avctx)
{
    int ret;
    FLIF16DecoderContext *s = avctx->priv_data;

    if (!s->maniac_ctx.forest) {
        __PLN__
        s->maniac_ctx.forest = av_mallocz((s->channels) *
                                          sizeof(*(s->maniac_ctx.forest)));
        if (!s->maniac_ctx.forest) {
            av_log(avctx, AV_LOG_ERROR, "could not allocate \n");
            return AVERROR(ENOMEM);
        }
        s->segment = s->i = 0; // Remove later
    }

    switch (s->segment) {
        case 0:
            loop:
            if (s->i >= s->channels)
                goto end;
            /*
            ff_flif16_maniac_ni_prop_ranges_init(s->prop_ranges,
                                                 &s->prop_ranges_size, s->ranges,
                                                 s->i, s->channels);*/
            __PLN__
            ++s->segment;

        case 1:
            ret = ff_flif16_read_maniac_tree(s->rc, &s->maniac_ctx, s->prop_ranges,
                                             s->prop_ranges_size, s->i);
            if (ret)
                goto end;
            av_free(s->prop_ranges);
            --s->segment;
            __PLN__
            goto loop;
    }

    end:
    s->state = FLIF16_PIXELDATA;
    return ret;
}

static int ff_flif16_read_ni_image(void)
{
    return 0;
}

static int ff_flif16_read_pixeldata(AVCodecContext *avctx, AVFrame *p)
{
    return AVERROR_EOF;
}

static int ff_flif16_read_checksum(AVCodecContext *avctx)
{
    return AVERROR_EOF;
}

static int flif16_decode_frame(AVCodecContext *avctx,
                               void *data, int *got_frame,
                               AVPacket *avpkt)
{
    int ret = AVERROR(EINVAL);
    FLIF16DecoderContext *s = avctx->priv_data;
    const uint8_t *buf      = avpkt->data;
    int buf_size            = avpkt->size;
    AVFrame *p              = data;
    printf("[Decode] Packet Size = %d\n", buf_size);
    bytestream2_init(&s->gb, buf, buf_size);
    __PLN__
    // Looping is done to change states in between functions.
    // Function will either exit on AVERROR(EAGAIN) or AVERROR_EOF
    do {
        switch(s->state) {
            case 0: case FLIF16_HEADER:
                ret = ff_flif16_read_header(avctx);
                break;

            case FLIF16_SECONDHEADER:
                ret = ff_flif16_read_second_header(avctx);
                break;

            case FLIF16_TRANSFORM:
                ret = ff_flif16_read_transforms(avctx);
                break;

            case FLIF16_MANIAC:
                ret = ff_flif16_read_maniac_forest(avctx);
                break;

            case FLIF16_PIXELDATA:
                __PLN__
                ret = ff_flif16_read_pixeldata(avctx, p);
                break;

            case FLIF16_CHECKSUM:
                ret = ff_flif16_read_checksum(avctx);
                break;
        }
    } while (!ret);

    printf("[Decode Result]\n"                  \
           "Width: %u, Height: %u, Frames: %u\n"\
           "ia: %x bpc: %u channels: %u\n"      \
           "alphazero: %u custombc: %u\n"       \
           "cutoff: %u alphadiv: %u \n"         \
           "loops: %u\n", s->width, s->height, s->frames, s->ia, s->bpc,
           s->channels, s->alphazero, s->custombc, s->cut,
           s->alpha, s->loops);

    if (s->framedelay) {
        printf("Framedelays:\n");
        for(uint32_t i = 0; i < s->frames; ++i)
            printf("%u, ", s->framedelay[i]);
        printf("\n");
    }

    if(s->maniac_ctx.forest) {
        printf("MANIAC Tree first node:\n" \
               "property value: %d\n", s->maniac_ctx.forest[0]->data[0].property);
    }
    return ret;
}

static av_cold int flif16_decode_end(AVCodecContext *avctx)
{
    // TODO complete function
    FLIF16DecoderContext *s = avctx->priv_data;
    av_free(s->rc);
    if(s->framedelay)
        av_free(s->framedelay);
    return 0;
}

AVCodec ff_flif16_decoder = {
    .name           = "flif16",
    .long_name      = NULL_IF_CONFIG_SMALL("FLIF (Free Lossless Image Format)"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FLIF16,
    .close          = flif16_decode_end,
    .priv_data_size = sizeof(FLIF16DecoderContext),
    .decode         = flif16_decode_frame,
    //.capabilities   = 0,
    //.caps_internal  = 0,
    .priv_class     = NULL,
};
