/*
 * FLIF16 Encoder
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

/*
 * In the case of the encoder, we have to first of all copy all frames to our
 * pixeldata array, determine the header components and write our image in the
 * encoder's flush mode.
 */

/**
 * @file
 * FLIF16 Decoder
*/

#include "flif16.h"
#include "flif16_rangecoder.h"
#include "flif16_transform.h"

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"

/**
 * The size that the frame/pixeldata array will be initialized with
 */
#define FRAMES_BASE_SIZE 16

typedef enum FLIF16EncodeStates {
    FLIF16_HEADER = 0,
    FLIF16_SECONDHEADER,
    FLIF16_TRANSFORM,
    FLIF16_OUTPUT,
    FLIF16_EOS
} FLIF16States;

typedef struct FLIF16EncoderContext {

    /* Inheritance from FLIF16Context */

    FLIF16MANIACContext maniac_ctx;
    FLIF16RangeCoder rc;

    // Dimensions
    uint32_t width;
    uint32_t height;
    uint32_t num_frames;
    uint32_t meta;       ///< Size of a meta chunk

    // Primary Header
    uint32_t bpc;         ///< 2 ^ Bits per channel - 1
    uint16_t *framedelay; ///< Frame delay for each frame
    uint8_t  ia;          ///< Is image interlaced or/and animated or not
    uint8_t  num_planes;  ///< Number of planes
    uint8_t  loops;       ///< Number of times animation loops
    uint8_t  plane_mode[MAX_PLANES];

    // Transform flags
    uint8_t framedup;
    uint8_t frameshape;
    uint8_t framelookback;

    /* End Inheritance from FLFIF16Context */

    PutByteContext pb;
    AVFrame *in_frame;
    AVPacket *out_packet;
    FLIF16PixelData *frames;

    FLIF16EncodeStates state;
    uint16_t *framepts;
    uint16_t frames_size;
    uint8_t interlaced; ///< Flag. Is image interlaced?
    
} FLIF16EncoderContext;

static flif16_determine_header(AVCodecContext *avctx)
{
    FLIF16EncoderContext *s = avctx->priv_data;
    s->bpc = 0xFF;
    s->width  = avctx->width;
    s->height = avctx->height;

    // Determine depth and number of planes
    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_GRAY16:
        s->bpc = 0xFFFF;
    case AV_PIX_FMT_GRAY8:
        s->num_planes = 1;
        break;

    case AV_PIX_FMT_RGB48:
        s->bpc = 0xFFFF;
    case AV_PIX_FMT_RGB24:
        s->num_planes = 3;
        break;

    case AV_PIX_FMT_RGBA64:
        s->bpc = 0xFFFF;
    case AV_PIX_FMT_RGB32:
        s->num_planes = 4;
        break;
    }

    s->frames = ff_flif16_frames_init(1);
    s->frames_size = 1;
    s->state = FLIF16_SECONDHEADER;
    return AVERROR_EOF;
}

static inline void varint_write(PutByteContext *pb, uint32_t num)
{
    while (num > 127) {
        bytestream2_put_byte(pb, (uint8_t) (num | 128));
        num >>= 7;
    }
    bytestream2_put_byte(pb, num);
}

static int flif16_copy_pixeldata(AVCodecContext *avctx)
{
    FLIF16EncoderContext *s = avctx->priv_data;
    uint64_t temp;

    if (s->num_frames > 1) {
        if (!s->framepts) {
            s->framepts = av_malloc_array(FRAMES_BASE_SIZE, sizeof(*s->framepts));
            s->frames   = ff_flif16_frames_resize(s->frames, s->frames_size, FRAMES_BASE_SIZE);
            s->frames_size = FRAMES_BASE_SIZE;
        }
        if (s->num_frames + 1 >= s->frames_size) {
            s->framepts = av_realloc_f(s->frame, s->frames_size * 2,
                                       sizeof(*s->framepts));
            s->frames   = ff_flif16_frames_resize(s->frames, s->frames_size,
                                                  s->frames_size * 2);
            s->frames_size *= 2;
        }
        s->framepts[s->num_frames] = s->curr_frame->pts;
    }

    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_GRAY8:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                PIXEL_SET(s, s->num_frames, 0, i, j, *(s->in_frame->data[0] + i * s->in_frame->linesize[0] + j));
            }
        }
        break;

    case AV_PIX_FMT_RGB24:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                PIXEL_SET(s, s->num_frames, 0, i, j, *(s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 3 + 0));
                PIXEL_SET(s, s->num_frames, 1, i, j, *(s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 3 + 1));
                PIXEL_SET(s, s->num_frames, 2, i, j, *(s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 3 + 2));
            }
        }
        break;

    case AV_PIX_FMT_RGB32:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                  temp = *((uint32_t *) (s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 4))
                  PIXEL_SET(s, s->num_frames, 3, i, j, (uint8_t) temp >> 24);
                  PIXEL_SET(s, s->num_frames, 0, i, j, (uint8_t) temp >> 16);
                  PIXEL_SET(s, s->num_frames, 1, i, j, (uint8_t) temp >> 8);
                  PIXEL_SET(s, s->num_frames, 2, i, j, (uint8_t) temp);
            }
        }
        break;

    case AV_PIX_FMT_GRAY16:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                PIXEL_SET(s, s->num_frames, 0, i, j,
                          *((uint16_t *) (s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 2)));
            }
        }
        break;

    case AV_PIX_FMT_RGB48:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                PIXEL_SET(s, s->num_frames, 0, i, j, *((uint16_t *) (s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 6 + 0)));
                PIXEL_SET(s, s->num_frames, 1, i, j, *((uint16_t *) (s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 6 + 1)));
                PIXEL_SET(s, s->num_frames, 2, i, j, *((uint16_t *) (s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 6 + 2)));
            }
        }

    case AV_PIX_FMT_RGBA64:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                temp = *((uint64_t *) (s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 8))
                PIXEL_SET(s, s->num_frames, 3, i, j, (uint16_t) temp >> 48);
                PIXEL_SET(s, s->num_frames, 2, i, j, (uint16_t) temp >> 32);
                PIXEL_SET(s, s->num_frames, 1, i, j. (uint16_t) temp >> 16);
                PIXEL_SET(s, s->num_frames, 0, i, j, (uint16_t) temp);
            }
        }
        break;

    default:
        av_log(avctx, AV_LOG_FATAL, "Pixel format %d out of bounds?\n", avctx->pix_fmt);
        return AVERROR_PATCHWELCOME;
    }

    s->num_frames++;
    s->state = FLIF16_TRANSFORM;
    return AVERROR_EOF;
}

static int flif16_determine_secondheader(AVCodecContext *avctx)
{
    // Read all frames, copy to pixeldata array, determine pts, determine duration,
    // On next step, determine transforms
    int ret;
    FLIF16EncoderContext *s = avctx->priv_data;
    if (ret = flif16_copy_pixeldata(avctx) < 0)
        return ret;
    s->frames = ff_flif16_frames_resize(s->frames, s->frames_size,
                                        s->num_frames);
    s->framepts = av_realloc_f(s->frame, s->num_frames, sizeof(*s->framepts));
    return AVERROR_EOF;
}

static int flif16_determine_transforms(AVCodecContext * avctx)
{
    // From pixeldata determine transforms, fill up transform array.
    // Make ranges
    return AVERROR_EOF;
}

static int flif16_determine_maniac_forest(AVCodecContext * avctx)
{
    // This involves a single pass pixel encoding for making the MANIAC tree
    // for the image.
    return AVERROR_EOF;
}

static int flif16_write_stream(AVCodecContext * avctx)
{
    // Write the whole file in this section: header, secondheader, transforms,
    // rough pixeldata, maniac tree, pixeldata and checksum.
    FLIF16EncoderContext *s = avctx->priv_data;

    switch (s->segment) {
    case 0:
        // Uncoded data
        
        // Write magic number
        bytestream2_put_buffer(&s->pb, flif16_header, FF_ARRAY_ELEMS(flif16_header));
        // Write interlaced/animated/num_planes flag
        bytestream2_put_byte(&s->pb, (uint8_t) ((3 + (s->interlaced * 2) +
                             (s->num_frames > 1)) << 4) & s->num_planes);
        // Write bpc flag
        bytestream2_put_byte(&s->pb, s->bpc > 255 ? '2' : '1');
        varint_write(&s->pb, s->width - 1);
        varint_write(&s->pb, s->height - 1);
        varint_write(&s->pb, s->frames > 1 ? s->frames - 2 : 1);

        // TODO handle metadata

        // Start of encoded bytestream
        bytestream2_put_byte(&s->pb, 0);
        s->segment++;

    case 1:
        // Coded data

        // Second header

        // We don't write the custom bpc segment.

        // Alphazero flag
        if (s->num_planes > 3)
            RAC_PUT(s->rc, NULL, 0, 1, s->alphazero, FLIF16_RAC_UNI_INT8);

        if (s->num_frames > 1) {
            // Loops
            RAC_PUT(s->rc, NULL, 0, 100, 0, FLIF16_RAC_UNI_INT8);

            // Frame delay TODO handle duration from demuxer
            for (int i = 0; i < s->num_frames - 1; i++)
                RAC_PUT(s->rc, NULL, 0, 60000, s->framepts[i], FLIF16_RAC_UNI_INT16);
        }
        
        // Custom Alpha
        RAC_PUT(s->rc, NULL, 0, 1, 0, FLIF16_RAC_UNI_INT8);

        // Custom bitchance may be present if custom alpha is present.
        s->segment++;

    case 2:
        // Transforms

        // We will just write the transform here
        /*
         * for (int i = 0; i < s->transforms_top; i++) {
         *     RAC_PUT(&s->rc, NULL, 0, 0, 1, FLIF16_RAC_BIT);
         *     RAC_PUT(s->rc, NULL, 0, 13, i, FLIF16_RAC_UNI_INT8);
         *     ff_flif16_transforms_write()
         * }
         *
         * RAC_PUT(&s->rc, NULL, 0, 0, 0, FLIF16_RAC_BIT);
         */
         s->segment++;

    case 3:
        // Rough pixeldata
        /*
         * if (s->interlaced)
         *     flif16_write_rough_pixeldata()
         */
         s->segment++;

    case 4:
        // MANIAC Tree
        /*
         * flif16_write_maniac_forest()
         */
         s->segment++;

    case 5:
        // Actual Pixeldata
        /*
         * flif16_write_pixeldata()
         */
        s->segment++;

    case 6:
        // Write Checksum?
        /*
         * flfi16_write_crc32_checksum();
         */
    }
    
    return AVERROR_EOF;
}

static int flif16_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                               const AVFrame *frame, int *got_packet)
{
    int ret = 0;
    FLIF16EncoderContext *s = avctx->priv_data;
    s->curr_frame = frame;

    do {
        switch (s->state) {
        case FLIF16_HEADER:
            ret = flif16_determine_header(avctx);
            break;

        case FLIF16_SECONDHEADER:
            ret = flif16_determine_secondheader(avctx);
            break;

        case FLIF16_TRANSFORM:
            ret = flif16_determine_transforms(avctx);
            break;

        case FLIF16_MANIAC:
            ret = flif16_determine_maniac_forest(avctx);
            break;

        case FLIF16_OUTPUT:
            ret = flif16_write_packet(avctx);
            break;

        case FLIF16_EOS:
            ret = AVERROR_EOF;
            break;
        }
    } while (!ret);

    return ret;
}

static int flif16_encode_end(AVCodecContext *avctx)
{
    FLIF16EncoderContext *s = avctx->priv_data;
    
    return 0;
}


AVCodec ff_flif16_encoder = {
    .name           = "flif16",
    .long_name      = NULL_IF_CONFIG_SMALL("FLIF (Free Lossless Imange Format)"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FLIF16,
    .priv_data_size = sizeof(FLIF16EncoderContext),
    .close          = flif16_encode_end,
    .encode2        = flif16_encode_frame,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB32,
        AV_PIX_FMT_RGB48, AV_PIX_FMT_RGBA64,
    }
};
