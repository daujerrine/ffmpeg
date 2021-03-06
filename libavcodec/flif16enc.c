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
 * FLIF16 Encoder
*/

#include "flif16.h"
#include "flif16_rangecoder.h"
#include "flif16_rangecoder_enc.h"
#include "flif16_transform.h"

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"

#include <signal.h>

//#define PRINT_LINE printf("At: %s, %s %d\n", __func__, __FILE__, __LINE__);
#define PRINT_LINE 
/**
 * The size that the frame/pixeldata array will be initialized with
 */
#define FRAMES_BASE_SIZE 16

typedef enum FLIF16EncodeStates {
    FLIF16_HEADER = 0,
    FLIF16_SECONDHEADER,
    FLIF16_TRANSFORM,
    FLIF16_MANIAC,
    FLIF16_OUTPUT,
    FLIF16_EOS
} FLIF16EncodeStates;

typedef struct FLIF16EncoderContext {

    /* Inheritance from FLIF16Context */

    FLIF16MANIACContext maniac_ctx;
    FLIF16RangeCoder rc;

    // Dimensions
    uint32_t width;
    uint32_t height;
    uint32_t meta;       ///< Size of a meta chunk
    uint32_t num_frames;

    // Primary Header
    uint16_t *framedelay; ///< Frame delay for each frame
    uint32_t bpc;         ///< 2 ^ Bits per channel - 1
    uint8_t  ia;          ///< Is image interlaced or/and animated or not
    uint8_t  num_planes;  ///< Number of planes
    uint8_t  loops;       ///< Number of times animation loops
    FLIF16PlaneMode  plane_mode[MAX_PLANES];

    // Transform flags
    uint8_t framedup;
    uint8_t frameshape;
    uint8_t framelookback;

    /* End Inheritance from FLFIF16Context */

    PutByteContext pb;
    AVFrame *in_frame;
    AVPacket *out_packet;
    FLIF16PixelData *frames;
    AVRational relative_timebase;
    int64_t final_frame_duration;

    FLIF16EncodeStates state;
    uint16_t *framepts;
    uint16_t frames_size;
    uint8_t interlaced; ///< Flag. Is image interlaced?
    uint8_t alphazero;

    // State management variables
    int segment;
    int segment2;
    int i;
    int i2;
    int i3;
    
} FLIF16EncoderContext;

static int flif16_determine_header(AVCodecContext *avctx)
{
    int ret;
    FLIF16EncoderContext *s = avctx->priv_data;
    s->plane_mode[0] = FLIF16_PLANEMODE_NORMAL;
    s->plane_mode[1] = FLIF16_PLANEMODE_NORMAL;
    s->plane_mode[2] = FLIF16_PLANEMODE_NORMAL;
    s->plane_mode[3] = FLIF16_PLANEMODE_NORMAL;
    s->plane_mode[4] = FLIF16_PLANEMODE_NORMAL;
    
    s->bpc = 0xFF;
    s->width  = avctx->width;
    s->height = avctx->height;
    PRINT_LINE
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

    /*
     * TODO determine planes from the given pixfmt.
     * Seems there is no other way to do this other than heuristically.
     */

    // We set it to MAX_PLANES if lookback plane is required
    s->num_planes = 4;

    // Check for multiplication overflow
    if ((ret = av_image_check_size2(s->width, s->height, avctx->max_pixels,
        avctx->pix_fmt, 0, avctx)) < 0)
        return ret;
    PRINT_LINE
    s->frames = ff_flif16_frames_init(1);
    PRINT_LINE
    s->frames_size = 1;
    PRINT_LINE
    s->state = FLIF16_SECONDHEADER;
    return 0;
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
    int32_t const_plane_value[MAX_PLANES] = {0};
    PRINT_LINE
    if (!s->in_frame) {
        // We have processed all the frames
        return 0;
    }
    PRINT_LINE
    if (s->num_frames > 0) {

        if (!s->framepts) {
            s->framepts = av_malloc_array(FRAMES_BASE_SIZE, sizeof(*s->framepts));
            if (!s->framepts)
                return AVERROR(ENOMEM);
            s->frames = ff_flif16_frames_resize(s->frames, s->frames_size, FRAMES_BASE_SIZE);
            if (!s->frames)
                return AVERROR(ENOMEM);
            s->frames_size = FRAMES_BASE_SIZE;
            s->framepts[0] = 0;
        }

        if (s->num_frames + 1 >= s->frames_size) {
            printf("REALLOCATING %d -> %d\n", s->frames_size, s->frames_size * 2);
            s->framepts = av_realloc_f(s->framepts, s->frames_size * 2,
                                       sizeof(*s->framepts));
            if (!s->framepts)
                return AVERROR(ENOMEM);
            s->frames = ff_flif16_frames_resize(s->frames, s->frames_size,
                                                  s->frames_size * 2);
            if (!s->frames)
                return AVERROR(ENOMEM);
            s->frames_size *= 2;
        }

        // raise(SIGINT);

        s->final_frame_duration = s->in_frame->pkt_duration;
        s->framepts[s->num_frames] = s->in_frame->pts;
        printf(">>>> %d\n", s->in_frame->pts);

        /*
         * This will allow for conversion between frame durations
         * This is done mostly to accomodate for the last frame's duration
         * without having to do convoluted demuxer stuff like copy pasting the
         * whole of the rangecoder state to the packet and sending it over to
         * add in the time stamp. Even then it wouldn't be possible I think.
         */

        s->relative_timebase.num = s->in_frame->pts;
        s->relative_timebase.den = s->in_frame->best_effort_timestamp;
    }
 
    ff_flif16_planes_init(CTX_CAST(s), &s->frames[s->num_frames],
                          const_plane_value);

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
                  temp = *((uint32_t *) (s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 4));
                  PIXEL_SET(s, s->num_frames, 3, i, j, (uint8_t) (temp >> 24));
                  PIXEL_SET(s, s->num_frames, 0, i, j, (uint8_t) (temp >> 16));
                  PIXEL_SET(s, s->num_frames, 1, i, j, (uint8_t) (temp >> 8));
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
                temp = *((uint64_t *) (s->in_frame->data[0] + i * s->in_frame->linesize[0] + j * 8));
                PIXEL_SET(s, s->num_frames, 3, i, j, (uint16_t) (temp >> 48));
                PIXEL_SET(s, s->num_frames, 2, i, j, (uint16_t) (temp >> 32));
                PIXEL_SET(s, s->num_frames, 1, i, j, (uint16_t) (temp >> 16));
                PIXEL_SET(s, s->num_frames, 0, i, j, (uint16_t) temp);
            }
        }
        break;

    default:
        av_log(avctx, AV_LOG_FATAL, "Pixel format %d out of bounds?\n", avctx->pix_fmt);
        return AVERROR_PATCHWELCOME;
    }

    PRINT_LINE
    s->num_frames++;
    return AVERROR(EAGAIN);
}

static int flif16_determine_secondheader(AVCodecContext *avctx)
{
    // Read all frames, copy to pixeldata array, determine pts, determine duration,
    // On next step, determine transforms
    int ret;
    FLIF16EncoderContext *s = avctx->priv_data;
    ret = flif16_copy_pixeldata(avctx);
    if (ret) {
        PRINT_LINE
        return ret;
    }

    ff_flif16_rac_enc_init(&s->rc, &s->pb);
    s->state = FLIF16_OUTPUT;
    s->frames = ff_flif16_frames_resize(s->frames, s->frames_size,
                                        s->num_frames);
    if (s->num_frames > 1)
        s->framepts = av_realloc_f(s->framepts, s->num_frames,
                                    sizeof(*s->framepts));
    return 0;
}

static int flif16_determine_transforms(AVCodecContext * avctx)
{
    // From pixeldata determine transforms, fill up transform array.
    // Make ranges
    return AVERROR_EOF;
}


/*
 * Here's how the rest of the data (non interlaced) is coded.
 * 1. We first perform an encoding pass. In this phase we do not write rangecoder
 *    output to the file. We use a symbol coder which is made to generate the
 *    maniac tree (PropertySymbolCoder used).
 * 2. We repeat this process REPEAT number of times. More REPEAT, more quality.
 * 3. We then simply the MANIAC tree that has been generated.
 * 4. We then write the MANIAC tree to the file.
 * 5. We now encode the actual pixel data with the second pass.
 *    FinalPropertySymbolCoder is used in this case.
 *
 * The process is similar for interlaced images, except that we use interlacing
 * offsets for pixels, stop at rough_zl, encode rough pixeldata with the empty
 * tree, then perform the second encoding pass, generate the MANIAC tree,
 * and finally do the the third pass where we encode rough_zl to 0.
 */

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
        PRINT_LINE
        // Write magic number
        bytestream2_put_buffer(&s->pb, flif16_header, FF_ARRAY_ELEMS(flif16_header));
        // Write interlaced/animated/num_planes flag
        printf("numplanes flag: %x %x : %x\n", (3 + (s->interlaced * 2) + (s->num_frames > 1)),
               s->num_planes, (uint8_t) ((3 + (s->interlaced * 2) +
                             (s->num_frames > 1)) << 4) | s->num_planes );
        bytestream2_put_byte(&s->pb, (uint8_t) ((3 + (s->interlaced * 2) +
                             (s->num_frames > 1)) << 4) | s->num_planes);
        // Write bpc flag
        bytestream2_put_byte(&s->pb, s->bpc > 255 ? '2' : '1');
        varint_write(&s->pb, s->width - 1);
        varint_write(&s->pb, s->height - 1);

        if (s->num_frames > 1)
            varint_write(&s->pb, s->num_frames > 1 ? s->num_frames - 2 : 1);

        // TODO handle metadata

        // Start of encoded bytestream
        bytestream2_put_byte(&s->pb, 0);
        s->segment++;

    case 1:
        // Coded data
        PRINT_LINE
        // Second header

        // We don't write the custom bpc segment.

        // Alphazero flag
        if (0)
            RAC_PUT(&s->rc, NULL, 0, 1, s->alphazero, FLIF16_RAC_UNI_INT8);

        if (s->num_frames > 1) {
            // Loops
            RAC_PUT(&s->rc, NULL, 0, 100, 0, FLIF16_RAC_UNI_INT8);

            for (int i = 0; i < s->num_frames - 1; i++) {
                RAC_PUT(&s->rc, NULL, 0, 60000,  s->framepts[i+1] -
                        s->framepts[i], FLIF16_RAC_UNI_INT16);
                printf("fr: %d %d %d\n", i + 1, s->framepts[i + 1] - s->framepts[i], s->framepts[i]);
            }

            if (s->final_frame_duration &&
                s->relative_timebase.num &&
                s->relative_timebase.den) {
                // Now handle duration of final frame from given input frame
                // duration.
                RAC_PUT(&s->rc, NULL, 0, 60000, s->final_frame_duration  *
                                                s->relative_timebase.num /
                                                s->relative_timebase.den,
                                                FLIF16_RAC_UNI_INT16);
                printf("fr: %d %ld\n", s->num_frames, s->final_frame_duration  *
                                                     s->relative_timebase.num /
                                                     s->relative_timebase.den);
            } else {
                // Input frame duration has not been set, Take duration from
                // previous frame and copy it over. This is a compromise.
                RAC_PUT(&s->rc, NULL, 0, 60000, s->framepts[s->num_frames - 1] -
                s->framepts[s->num_frames - 2], FLIF16_RAC_UNI_INT16);
                printf("fr: %d %d\n", s->num_frames, s->framepts[s->num_frames - 1] -
                                                     s->framepts[s->num_frames - 2]);
            }
        }
        // Custom bitchance may be present if custom alpha is present.
        s->segment++;

    case 2:
        // Transforms
        PRINT_LINE
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
         *
         * if (s->interlaced)
         *     flif16_write_rough_pixeldata()
         */

         // ff_flif16_
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
         s->segment++;

    case 7:
        // Flush the RAC
        if (!ff_flif16_rac_enc_flush(&s->rc))
            goto need_more_buffer;
    }

    s->segment = 0;
    s->state = FLIF16_EOS;
    return 0;

need_more_buffer:
    printf("Need more buffer\n");
    return AVERROR(EAGAIN);
}

static int flif16_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                               const AVFrame *frame, int *got_packet)
{
    int ret = 0;
    FLIF16EncoderContext *s = avctx->priv_data;
    PRINT_LINE
    if (frame) {
        av_frame_ref(s->in_frame, frame);
        printf("pts: %lu "
               "pkt_duration: %lu "
               "keyframe: %d "
               "pkt_dts: %lu "
               "DPN: %d "
               "pkt_pos: %lu "
               "beTS: %lu\n",
               frame->pts, frame->pkt_duration, frame->key_frame, frame->pkt_dts,
               frame->display_picture_number, frame->pkt_pos, frame->best_effort_timestamp);
    } else {
        printf("Entering encoder draining mode\n");
        printf("pkt_timebase %d/%d\n", avctx->pkt_timebase.num, avctx->pkt_timebase.den);
        av_frame_unref(s->in_frame);
        av_frame_free(&s->in_frame);
        s->in_frame = NULL;
    }
    
    do {
        switch (s->state) {
        case FLIF16_HEADER:
            ret = flif16_determine_header(avctx);
            break;

        case FLIF16_SECONDHEADER:
            ret = flif16_determine_secondheader(avctx);
            if (ret < 0 && ret == AVERROR(EAGAIN)) {
                PRINT_LINE
                av_packet_unref(pkt);
                return 0;
            }
            break;

        case FLIF16_TRANSFORM:
            ret = flif16_determine_transforms(avctx);
            break;

        case FLIF16_MANIAC:
            ret = flif16_determine_maniac_forest(avctx);
            break;

        case FLIF16_OUTPUT:
            PRINT_LINE
            if ((ret = ff_alloc_packet2(avctx, pkt, 10000 + AV_INPUT_BUFFER_MIN_SIZE, 0)) < 0)
                return ret;
            bytestream2_init_writer(&s->pb, pkt->data, pkt->size);
            printf("bs init %d\n", pkt->size);
            ret = flif16_write_stream(avctx);
            if (!ret) {
                *got_packet = 1;
                av_shrink_packet(pkt, bytestream2_tell_p(&s->pb));
                printf("bs tellp %d %d\n", bytestream2_tell_p(&s->pb), pkt->size);
                pkt->pts = 0;
                return 0;
            }
            break;

        case FLIF16_EOS:
            ret = AVERROR_EOF;
            break;
        }
    } while (!ret);

    return ret;
}

static int flif16_encode_init(AVCodecContext *avctx)
{
    FLIF16EncoderContext *s = avctx->priv_data;
    s->in_frame = av_frame_alloc();
    avctx->time_base.num = 1;
    avctx->time_base.den = 1000;
    if (!s->in_frame)
        return AVERROR(ENOMEM);
    return 0;
}

static int flif16_encode_end(AVCodecContext *avctx)
{
    return 0;
}


AVCodec ff_flif16_encoder = {
    .name           = "flif16",
    .long_name      = NULL_IF_CONFIG_SMALL("FLIF (Free Lossless Image Format)"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FLIF16,
    .priv_data_size = sizeof(FLIF16EncoderContext),
    .init           = flif16_encode_init,
    .close          = flif16_encode_end,
    .encode2        = flif16_encode_frame,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB32,
        AV_PIX_FMT_RGB48, AV_PIX_FMT_RGBA64,
        AV_PIX_FMT_NONE
    }
};
