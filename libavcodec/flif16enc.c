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

typedef struct FLIF16EncoderContext {

    /* Inheritance from FLIF16Context */

    FLIF16MANIACContext maniac_ctx;
    FLIF16RangeCoder rc;
    PutByteContext pb;

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

    
} FLIF16EncoderContext;

static int flif16_enc_frame(AVCodecContext *avctx, AVPacket *pkt,
                      const AVFrame *pict, int *got_packet)
{
    int ret = 0;
    FLIF16EncoderContext *s = avctx->priv_data;
    const uint8_t *buf      = avpkt->data;
    int buf_size            = avpkt->size;
    AVFrame *p              = data;

    do {
        switch (s->state) {
        case FLIF16_HEADER:
            ret = flif16_write_header(avctx);
            break;

        case FLIF16_SECONDHEADER:
            ret = flif16_write_secondheader(avctx);
            break;
        }
    } while (!ret);

    return ret;
}

static int flif16_encode_end(AVCodecContext *avctx)
{
    
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
    .capabilities   = AV_CODEC_CAP_ENCODER_FLUSH,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB32,
        AV_PIX_FMT_RGB48, AV_PIX_FMT_RGBA64,
    }
};
