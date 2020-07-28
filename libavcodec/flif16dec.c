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
#include "flif16_transform.h"

#include "avcodec.h"
#include "libavutil/common.h"
#include "bytestream.h"
#include "avcodec.h"
#include "internal.h"

#define __SUBST__
/*\
for(int k = 0; k < s->num_planes; ++k) {\
    for(int j = 0; j < s->height; ++j) {\
        for(int i = 0; i < s->width; ++i) {\
            printf("%d ", ff_flif16_pixel_get(CTX_CAST(s), &s->frames[0], k, j, i));\
        }\
        printf("\n");\
    }\
    printf("===\n");\
}*/

/*
 * Due to the nature of the format, the decoder has to take the entirety of the
 * data before it can generate any frames. The decoder has to return
 * AVERROR(EAGAIN) as long as the bitstream is incomplete.
 */


// TODO make variable size UNI_INT readers
typedef struct FLIF16DecoderContext {

    /* Inheritance from FLIF16Context */

    GetByteContext gb;
    FLIF16MANIACContext maniac_ctx;
    FLIF16RangeCoder rc;

    // Dimensions and other things.
    uint32_t width;
    uint32_t height;
    uint32_t num_frames;
    uint32_t meta;       ///< Size of a meta chunk

    // Primary Header     
    uint8_t  ia;         ///< Is image interlaced or/and animated or not
    uint32_t bpc;        ///< 2 ^ Bytes per channel
    uint8_t  num_planes; ///< Number of planes
    
    // change to uint8_t
    uint32_t loops;       ///< Number of times animation loops
    // change to uint32_t
    uint32_t *framedelay; ///< Frame delay for each frame

    uint8_t plane_mode[MAX_PLANES];

    // Transform flags
    uint8_t framedup;
    uint8_t frameshape;
    uint8_t framelookback;
    /* End Inheritance from FLIF16Context */

    FLIF16PixelData  *frames;
    uint32_t out_frames_count;
    AVFrame *out_frame;
    int64_t pts;
    
    uint8_t buf[FLIF16_RAC_MAX_RANGE_BYTES]; ///< Storage for initial RAC buffer
    uint8_t buf_count;    ///< Count for initial RAC buffer
    int state;            ///< The section of the file the parser is in currently.
    unsigned int segment; ///< The "segment" the code is supposed to jump to
    unsigned int segment2;///< The "segment" the code is supposed to jump to
    int i;                ///< A generic iterator used to save states between for loops.
    int i2;
    int i3;

    // Secondary Header
    uint8_t alphazero;    ///< Alphazero Flag
    uint8_t custombc;     ///< Custom Bitchance Flag
    uint8_t customalpha;  ///< Custom alphadiv & cutoff flag

    uint8_t cut;          ///< Chancetable custom cutoff
    uint32_t alpha;       ///< Chancetable custom alphadivisor
    uint8_t ipp;          ///< Invisible pixel predictor

    // Transforms
    // Size dynamically maybe
    FLIF16TransformContext *transforms[13];
    uint8_t transform_top;
    FLIF16RangesContext *range; ///< The minimum and maximum values a
                                ///  channel's pixels can take. Changes
                                ///  depending on transformations applied
    FLIF16RangesContext *prev_range;

    // MANIAC Trees
    int32_t (*prop_ranges)[2]; ///< Property Ranges
    uint32_t prop_ranges_size;
    
    // Pixeldata
    uint8_t curr_plane;        ///< State variable. Current plane under processing
    FLIF16ColorVal *grays;
    FLIF16ColorVal *properties;
    FLIF16ColorVal guess;      ///< State variable. Stores guess
    FLIF16ColorVal min, max;
    uint32_t c;                ///< State variable for current column

    // Interlaced Pixeldata
    int *zoomlevels;
    int zooms;
    int rough_zl;
    int quality;
    int scale;
    int *predictors;
    int breakpoints;
} FLIF16DecoderContext;

// Cast values to FLIF16Context for some functions.
#define CTX_CAST(x) ((FLIF16Context *) (x))

// TODO Remove PIXEL and PIXELY. Concerned with interlaced decoding
#define PIXEL(z,r,c) ff_flif16_pixel_getz(CTX_CAST(s), frame, p, z, r, c)
#define PIXELY(z,r,c) ff_flif16_pixel_getz(CTX_CAST(s), frame, FLIF16_PLANE_Y, z, r, c)

#define PIXEL_SET(ctx, fr, p, r, c, val) ff_flif16_pixel_set(CTX_CAST(ctx), &(ctx)->frames[fr], p, r, c, val)
#define PIXEL_GET(ctx, fr, p, r, c) ff_flif16_pixel_get(CTX_CAST(ctx), &(ctx)->frames[fr], p, r, c)
#define PIXEL_SETZ(ctx, fr, p, z, r, c, val) ff_flif16_pixel_setz(CTX_CAST(ctx), &(ctx)->frames[fr], p, z, r, c, val)
#define PIXEL_GETZ(ctx, fr, p, z, r, c) ff_flif16_pixel_getz(CTX_CAST(ctx), &(ctx)->frames[fr], p, z, r, c)
#define PIXEL_GETFAST(ctx, fr, p, r, c) ff_flif16_pixel_get(CTX_CAST(ctx), &(ctx)->frames[fr], p, r, c)
#define PIXEL_SETFAST(ctx, fr, p, r, c, val) ff_flif16_pixel_set(CTX_CAST(ctx), &(ctx)->frames[fr], p, r, c, val)

// If frame_dup exists, figure out what the previous frame actually is
#define PREV_FRAME(frames, f_no) (((frames)[(f_no) - 1].seen_before >= 0) ? &(frames)[(frames)[(f_no) - 1].seen_before] : &(frames)[(f_no) - 1])
#define PREV_FRAMENUM(frames, f_no) (((frames)[(f_no) - 1].seen_before >= 0) ? (frames)[(f_no) - 1].seen_before : (f_no) - 1)

// Static property values
static const int properties_ni_rgb_size[] = {7, 8, 9, 7, 7};
static const int properties_ni_rgba_size[] = {8, 9, 10, 7, 7};
static const int properties_rgb_size[] = {8, 10, 9, 8, 8};
static const int properties_rgba_size[] = {9, 11, 10, 8, 8};

// From reference decoder:
//
// The order in which the planes are encoded.
// lookback (Lookback) (animations-only, value refers to a previous frame) has
// to be first, because all other planes are not encoded if lookback != 0
// Alpha has to be next, because for fully transparent A=0 pixels, the other
// planes are not encoded
// Y (luma) is next (the first channel for still opaque images), because it is
// perceptually most important
// Co and Cg are in that order because Co is perceptually slightly more
// important than Cg [citation needed]
static const int plane_ordering[] = {4,3,0,1,2}; // lookback (lookback), A, Y, Co, Cg

enum FLIF16States {
    FLIF16_HEADER = 0,
    FLIF16_SECONDHEADER,
    FLIF16_TRANSFORM,
    FLIF16_ROUGH_PIXELDATA,
    FLIF16_MANIAC,
    FLIF16_PIXELDATA,
    FLIF16_OUTPUT,
    FLIF16_CHECKSUM,
    FLIF16_EOS
};

static int flif16_read_header(AVCodecContext *avctx)
{
    uint8_t temp, count = 4;
    FLIF16DecoderContext *s = avctx->priv_data;
    // TODO Make do without this array
    uint32_t *vlist[] = { &s->width, &s->height, &s->num_frames };
    
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    s->cut   = CHANCETABLE_DEFAULT_CUT;
    s->alpha = CHANCETABLE_DEFAULT_ALPHA;
    printf(">>> %u %u\n", s->cut, s->alpha);
    
    // Minimum size has been empirically found to be 8 bytes.
    if (bytestream2_size(&s->gb) < 8) {
        av_log(avctx, AV_LOG_ERROR, "buf size too small (%d)\n",
               bytestream2_size(&s->gb));
        return AVERROR(EINVAL);
    }
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);

    if (bytestream2_get_le32(&s->gb) != (*((uint32_t *) flif16_header))) {
        av_log(avctx, AV_LOG_ERROR, "bad magic number\n");
        return AVERROR(EINVAL);
    }

    s->state = FLIF16_HEADER;

    temp = bytestream2_get_byte(&s->gb);
    s->ia         = temp >> 4;
    s->num_planes = (0x0F & temp);
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);

    if (!(s->ia % 2)) {
        av_log(avctx, AV_LOG_ERROR, "interlaced images not supported\n");
        return AVERROR_PATCHWELCOME;
    }
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    
    s->bpc = bytestream2_get_byte(&s->gb);

    

    // Handle dimensions and frames
    for(int i = 0; i < 2 + ((s->ia > 4) ? 1 : 0); ++i) {
        while ((temp = bytestream2_get_byte(&s->gb)) > 127) {
            VARINT_APPEND(*vlist[i], temp);
            if (!(count--)) {
                av_log(avctx, AV_LOG_ERROR, "image dimensions too big\n");
                return AVERROR(ENOMEM);
            }
        }
        VARINT_APPEND(*vlist[i], temp);
        count = 4;
    }
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    s->width++;
    s->height++;
    (s->ia > 4) ? (s->num_frames += 2) : (s->num_frames = 1);

    if (s->num_frames > 1) {
        s->framedelay = av_mallocz(sizeof(*(s->framedelay)) * s->num_frames);
        if (!s->framedelay)
            return AVERROR(ENOMEM);
    }

    s->frames = ff_flif16_frames_init(CTX_CAST(s));

    if (!s->frames) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate frames\n");
        return AVERROR(ENOMEM);
    }
    
    // Handle Metadata Chunk. Currently it discards all data.

    while ((temp = bytestream2_get_byte(&s->gb)) != 0) {
        bytestream2_seek(&s->gb, 3, SEEK_CUR);
        // Read varint
        while ((temp = bytestream2_get_byte(&s->gb)) > 127) {
            VARINT_APPEND(s->meta, temp);
            if (!(count--)) {
                av_log(avctx, AV_LOG_ERROR, "metadata chunk too big \n");
                return AVERROR(ENOMEM);
            }
        }
        VARINT_APPEND(s->meta, temp);
        bytestream2_seek(&s->gb, s->meta, SEEK_CUR);
        count = 4;
    }

    printf("[%s] left = %d\n", __func__, bytestream2_get_bytes_left(&s->gb));
    s->state = FLIF16_SECONDHEADER;
    return 0;
}

static int flif16_read_second_header(AVCodecContext *avctx)
{
    uint32_t temp;
    FLIF16DecoderContext *s = avctx->priv_data;

    switch (s->segment) {
        case 0:
            s->buf_count += bytestream2_get_buffer(&s->gb, s->buf + s->buf_count,
                                                   FFMIN(bytestream2_get_bytes_left(&s->gb),
                                                   (FLIF16_RAC_MAX_RANGE_BYTES - s->buf_count)));
            // MSG("s->buf_count = %d buf = ", s->buf_count);
            for(int i = 0; i < FLIF16_RAC_MAX_RANGE_BYTES; ++i)
                printf("%x ", s->buf[i]);
            printf("\n");
            if (s->buf_count < FLIF16_RAC_MAX_RANGE_BYTES)
                return AVERROR(EAGAIN);

            ff_flif16_rac_init(&s->rc, &s->gb, s->buf, s->buf_count);
            ++s->segment;

        case 1:
            // In original source this is handled in what seems to be a very
            // bogus manner. It takes all the bpps of all planes and then
            // takes the max, negating any benefit of actually keeping these
            // multiple values.
            if (s->bpc == '0') {
                s->bpc = 0;
                for (; s->i < s->num_planes; ++s->i) {
                    RAC_GET(&s->rc, NULL, 1, 15, &temp, FLIF16_RAC_UNI_INT8);
                    s->bpc = FFMAX(s->bpc, (1 << temp) - 1);
                }
            } else
                s->bpc = (s->bpc == '1') ? 255 : 65535;
            s->i = 0;
            s->range = ff_flif16_ranges_static_init(s->num_planes, s->bpc);
            // MSG("planes : %d & bpc : %d\n", s->num_planes, s->bpc);
            ++s->segment;

        case 2:
            if (s->num_planes > 3) {
                RAC_GET(&s->rc, NULL, 0, 1, &s->alphazero,
                        FLIF16_RAC_UNI_INT8);
            }
            ++s->segment;

        case 3:
            if (s->num_frames > 1) {
                RAC_GET(&s->rc, NULL, 0, 100, &s->loops,
                        FLIF16_RAC_UNI_INT8);
            }
            ++s->segment;

        case 4:
            printf("s->segment = %d\n", s->segment);
            if (s->num_frames > 1) {
                for (; (s->i) < (s->num_frames); ++(s->i)) {
                    RAC_GET(&s->rc, NULL, 0, 60000, &(s->framedelay[s->i]),
                            FLIF16_RAC_UNI_INT16);
                }
                s->i = 0;
            }
            ++s->segment;

        case 5:
            // Has custom alpha flag
            RAC_GET(&s->rc, NULL, 0, 1, &s->customalpha, FLIF16_RAC_UNI_INT8);
            printf("has_custom_cutoff_alpha = %d\n", s->customalpha);
            ++s->segment;

        case 6:
            if (s->customalpha) {
                printf(">>>>>>>>>>>>>>custom cut\n");
                RAC_GET(&s->rc, NULL, 1, 128, &s->cut, FLIF16_RAC_UNI_INT8);
            }
            ++s->segment;

        case 7:
            if (s->customalpha) {
                RAC_GET(&s->rc, NULL, 2, 128, &s->alpha, FLIF16_RAC_UNI_INT8);
                s->alpha = 0xFFFFFFFF / s->alpha;
            }
            ++s->segment;

        case 8:
            if (s->customalpha)
                RAC_GET(&s->rc, NULL, 0, 1, &s->custombc, FLIF16_RAC_UNI_INT8);
            if (s->custombc) {
                av_log(avctx, AV_LOG_ERROR, "custom bitchances not implemented\n");
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

    ff_flif16_chancetable_init(&s->rc.ct, s->alpha, s->cut);

    printf("<<<<<<<<<< %d %d\n", s->alpha, s->cut);
    /*
    for(int i = 0; i < 4096; ++i)
        printf("%u ", s->rc.ct.zero_state[i]);
    printf("\n");
    for(int i = 0; i < 4096; ++i)
        printf("%u ", s->rc.ct.one_state[i]);
    printf("\n");
    */
    return 0;

    need_more_data:
    // MSG("Need more data\n");
    return AVERROR(EAGAIN);
}


static int flif16_read_transforms(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    FLIF16RangesContext *prev_range;
    uint8_t const_plane_value[MAX_PLANES];
    uint8_t temp;
    int unique_frames;

    loop:
    switch (s->segment) {
        case 0:
            RAC_GET(&s->rc, NULL, 0, 0, &temp, FLIF16_RAC_BIT);
            if(!temp)
                goto end;
            ++s->segment;

        case 1:
            RAC_GET(&s->rc, NULL, 0, 13, &temp, FLIF16_RAC_UNI_INT8);
            printf("Transform : %d\n", temp);
            if (!flif16_transforms[temp]) {
                av_log(avctx, AV_LOG_ERROR, "transform %u not implemented\n", temp);
                return AVERROR_PATCHWELCOME;
            }

            s->transforms[s->transform_top] = ff_flif16_transform_init(temp, s->range);
            if (!s->transforms[s->transform_top]) {
                av_log(avctx, AV_LOG_ERROR, "failed to initialise transform %u\n", temp);
                return AVERROR(ENOMEM);
            }
            
            printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            // TODO Replace with switch statement
            switch (temp) {
                case FLIF16_TRANSFORM_PALETTEALPHA:
                    s->plane_mode[FLIF16_PLANE_ALPHA] = FLIF16_PLANEMODE_CONSTANT;
                    ff_flif16_transform_configure(s->transforms[s->transform_top],
                                                  s->alphazero);

                case FLIF16_TRANSFORM_CHANNELCOMPACT:
                    if (s->num_planes > 3 && !s->plane_mode[FLIF16_PLANE_ALPHA])
                        s->plane_mode[FLIF16_PLANE_ALPHA] = FLIF16_PLANEMODE_FILL;

                case FLIF16_TRANSFORM_YCOCG:
                case FLIF16_TRANSFORM_PALETTE:
                    s->plane_mode[FLIF16_PLANE_Y] = FLIF16_PLANEMODE_NORMAL;
                    s->plane_mode[FLIF16_PLANE_CO] = FLIF16_PLANEMODE_NORMAL;
                    s->plane_mode[FLIF16_PLANE_CG] = FLIF16_PLANEMODE_NORMAL;
                    break;

                case FLIF16_TRANSFORM_DUPLICATEFRAME:
                    s->framedup = 1;
                    if(s->num_frames < 2)
                         return AVERROR(EINVAL);
                    ff_flif16_transform_configure(s->transforms[s->transform_top],
                                                  s->num_frames);
                    break;

                case FLIF16_TRANSFORM_FRAMESHAPE:
                    s->frameshape = 1;
                    if (s->num_frames < 2)
                        return AVERROR(EINVAL);
                    unique_frames = s->num_frames - 1;
                    for (unsigned int i = 0; i < s->num_frames; i++) {
                        if(s->frames[i].seen_before >= 0)
                            unique_frames--;
                    }
                    if (unique_frames < 1)
                        return AVERROR(EINVAL);
                    printf("unique_frames : %d\n", unique_frames);
                    ff_flif16_transform_configure(s->transforms[s->transform_top],
                                                  (unique_frames) * s->height);
                    ff_flif16_transform_configure(s->transforms[s->transform_top],
                                                  s->width);
                    break;

                case FLIF16_TRANSFORM_FRAMELOOKBACK:
                    if(s->num_frames < 2)
                        return AVERROR(EINVAL);
                    s->framelookback = 1;
                    ff_flif16_transform_configure(s->transforms[s->transform_top],
                                                  s->num_frames);
                    break;
            }
            ++s->segment;

        case 2:
            printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            //printf("%d\n", s->transforms[s->transform_top]->t_no);
            if(ff_flif16_transform_read(s->transforms[s->transform_top],
                                        CTX_CAST(s), s->range) <= 0)
                goto need_more_data;
            printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            prev_range = s->range;
            s->range = ff_flif16_transform_meta(CTX_CAST(s), s->frames, s->num_frames,
                                                s->transforms[s->transform_top],
                                                prev_range);
            if(!s->range)
                return AVERROR(ENOMEM);
            printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            printf("Ranges : %d\n", s->range->r_no);
            s->segment = 0;
            ++s->transform_top;
            goto loop;

        case 3:
            end:
            printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            s->segment = 3;   
            // Read invisible pixel predictor
            if (   s->alphazero && s->num_planes > 3
                && ff_flif16_ranges_min(s->range, 3) <= 0
                && !(s->ia % 2))
                RAC_GET(&s->rc, NULL, 0, 2, &s->ipp, FLIF16_RAC_UNI_INT8)
    }

    printf("[Resultant Ranges]\n");
    for (int i = 0; i < s->num_planes; ++i)
        printf("%d: %d, %d\n", i, ff_flif16_ranges_min(s->range, i),
        ff_flif16_ranges_max(s->range, i));
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    for (int i = 0; i < ((s->num_planes > 4) ? 4 : s->num_planes); ++i)
        if (   s->plane_mode[i] != FLIF16_PLANEMODE_NORMAL
            && (ff_flif16_ranges_min(s->range, i) >= ff_flif16_ranges_max(s->range, i))) {
            printf("Const value: %d %d\n", i, ff_flif16_ranges_min(s->range, i));
            const_plane_value[i] = ff_flif16_ranges_min(s->range, i);
        }
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    if (ff_flif16_planes_init(CTX_CAST(s), s->frames, s->plane_mode,
                              const_plane_value, s->framelookback) < 0) {
        av_log(avctx, AV_LOG_ERROR, "could not allocate planes\n");
        return AVERROR(ENOMEM);
    }
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);

    // if (!(s->ia % 2))
    //    s->state = FLIF16_ROUGH_PIXELDATA;
    // else
    //    s->state = FLIF16_MANIAC;
    s->state = FLIF16_MANIAC;
    s->segment = 0;
    return 0;

    need_more_data:
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    printf("need more data<>\n");
    return AVERROR(EAGAIN);
}

static int flif16_read_maniac_forest(AVCodecContext *avctx)
{
    int ret;
    FLIF16DecoderContext *s = avctx->priv_data;
    printf("called s->segment = %d ", s->segment);
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    if (!s->maniac_ctx.forest) {
        s->maniac_ctx.forest = av_mallocz((s->num_planes) * sizeof(*(s->maniac_ctx.forest)));
        if (!s->maniac_ctx.forest) {
            return AVERROR(ENOMEM);
        }
        s->segment = s->i = 0; // Remove later
    }
    printf("%d s->segment = %d\n", __LINE__, s->segment);
    printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    switch (s->segment) {
        case 0:
            loop:
            printf("channel: %d\n", s->i);
            if (s->i >= s->num_planes)
                goto end;

            if (!(s->ia % 2))
                s->prop_ranges = ff_flif16_maniac_prop_ranges_init(&s->prop_ranges_size, s->range,
                                                                   s->i, s->num_planes);
            else
                s->prop_ranges = ff_flif16_maniac_ni_prop_ranges_init(&s->prop_ranges_size, s->range,
                                                                      s->i, s->num_planes);

            printf("Prop ranges:\n");
            for(int i = 0; i < s->prop_ranges_size; ++i)
                printf("(%d, %d)\n", s->prop_ranges[i][0], s->prop_ranges[i][1]);
            if(!s->prop_ranges)
                return AVERROR(ENOMEM);
            ++s->segment;
            printf("%d s->segment = %d\n", __LINE__, s->segment);

        case 1:
            if (ff_flif16_ranges_min(s->range, s->i) >= ff_flif16_ranges_max(s->range, s->i)) {
                ++s->i;
                --s->segment;
                goto loop;
            }
            printf("%d s->segment = %d\n", __LINE__, s->segment);
            printf("Start:");
            for(unsigned int i = 0; i < s->prop_ranges_size; ++i)
                printf("%u: (%d, %d) ", i, s->prop_ranges[i][0], s->prop_ranges[i][1]);
            printf("\n");
            ret = ff_flif16_read_maniac_tree(&s->rc, &s->maniac_ctx, s->prop_ranges,
                                             s->prop_ranges_size, s->i);
            printf("Ret: %d\n", ret);
            if (ret) {
                goto error;
            }
            printf("%d s->segment = %d\n", __LINE__, s->segment);
            av_freep(&s->prop_ranges);
            --s->segment;
            ++s->i;
            printf("%d s->segment = %d\n", __LINE__, s->segment);
            goto loop;
    }

    end:
    printf("%d s->segment = %d\n", __LINE__, s->segment);
    s->state = FLIF16_PIXELDATA;
    s->segment = 0;
    return 0;

    error:
    return ret;
}

/* ============================================================================
 * Non interlaced plane decoding
 * ============================================================================
 */


static FLIF16ColorVal flif16_ni_predict_calcprops(FLIF16DecoderContext *s,
                                                  FLIF16PixelData *pixel,
                                                  FLIF16ColorVal *properties,
                                                  FLIF16RangesContext *ranges_ctx,
                                                  uint8_t p, uint32_t r,
                                                  uint32_t c, FLIF16ColorVal *min,
                                                  FLIF16ColorVal *max,
                                                  const FLIF16ColorVal fallback,
                                                  uint8_t nobordercases)
{
    FLIF16ColorVal guess, left, top, topleft, gradientTL;
    int width = s->width;
    int which = 0;
    int index = 0;
    if (p < 3) {
        for (int pp = 0; pp < p; pp++) {
            //printf("&a %d %d\n", index, ff_flif16_pixel_get(CTX_CAST(s), pixel, pp, r, c));
            properties[index++] = ff_flif16_pixel_get(CTX_CAST(s), pixel, pp, r, c); //image(pp,r,c);
        }
        if (ranges_ctx->num_planes > 3) {
            //printf("&b %d %d\n", index, ff_flif16_pixel_get(CTX_CAST(s), pixel, 3, r, c));
            properties[index++] = ff_flif16_pixel_get(CTX_CAST(s), pixel, 3, r, c); //image(3,r,c);
        }
    }
    left = (nobordercases || c > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r, c-1) : 
           (r > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c) : fallback));
    top = (nobordercases || r > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c) : left);
    topleft = (nobordercases || (r>0 && c>0) ? 
              ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c-1) : (r > 0 ? top : left));
    gradientTL = left + top - topleft;
    guess = MEDIAN3(gradientTL, left, top);
    printf("r %d %d %d\n", (c > 0) ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r, c-1) : 0,
                           (r > 0) ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c) : 0,
                           (r > 0 && c > 0) ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c-1) : 0);
    //printf("g %d\n", guess); 
    ff_flif16_ranges_snap(ranges_ctx, p, properties, min, max, &guess);
    //printf("g %d\n", guess); 
    //printf("min = %d max = %d\n", *min, *max);
    /*assert(min >= ff_flif16_ranges_min(ranges_ctx, p));
    assert(max <= ff_flif16_ranges_max(ranges_ctx, p));
    assert(guess >= min);
    assert(guess <= max);*/

    if (guess == gradientTL)
        which = 0;
    else if (guess == left)
        which = 1;
    else if (guess == top)
        which = 2;

    properties[index++] = guess;
    properties[index++] = which;

    if (nobordercases || (c > 0 && r > 0)) {
        //printf("&2a %d %d\n", index, left - topleft);
        properties[index++] = left - topleft;
        //printf("&2a %d %d\n", index, topleft - top);
        properties[index++] = topleft - top;
    } else {
        //printf("&2b %d 0\n", index);
        properties[index++] = 0;
        //printf("&2b %d 0\n", index);
        properties[index++] = 0; 
    }

    if (nobordercases || (c+1 < width && r > 0)) {
        //printf("&3a %d %d\n", index, top - ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c+1));
        properties[index++] = top - ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c+1); // top - topright 
    } else {
        //printf("&3b %d 0\n", index);
        properties[index++] = 0;
    }

    if (nobordercases || r > 1) {
        //printf("&4a %d %d\n", index, ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-2, c) - top);
        properties[index++] = ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-2, c) - top;  // toptop - top
    } else {
        //printf("&4b %d 0\n", index);
        properties[index++] = 0;
    }

    if (nobordercases || c > 1) {
        //printf("&5a %d %d\n", index, ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r, c-2) - left);
        properties[index++] = ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r, c-2) - left;  // leftleft - left
    } else {
        //printf("&5b %d 0\n", index);
        properties[index++] = 0;
    }

    //for (int i = 0; i < properties_ni_rgb_size[p]; ++i)
        //printf("%d ", properties[i]);
    //printf("\n");
    //printf("psl fallback = %d left = %d top = %d topleft = %d gradienttl = %d guess = %d\n", fallback, left, top, topleft, gradientTL, guess);
    //printf("p = %u r = %u c = %u min = %d max = %d\n", p, r, c, *min, *max);
    return guess;
}

static inline FLIF16ColorVal flif16_ni_predict(FLIF16DecoderContext *s,
                                               FLIF16PixelData *pixel,
                                               uint32_t p, uint32_t r, uint32_t c,
                                               FLIF16ColorVal gray) {
    FLIF16ColorVal left = (c > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r, c-1) :
                          (r > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c) : gray));
    FLIF16ColorVal top = (r > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r - 1, c) : left);
    FLIF16ColorVal topleft = (r > 0 && c > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r - 1, c - 1) : top);
    FLIF16ColorVal gradientTL = left + top - topleft;
    //printf("sl guess = %d\n", MEDIAN3(gradientTL, left, top));
    return MEDIAN3(gradientTL, left, top);
}

static int flif16_read_ni_plane(FLIF16DecoderContext *s,
                                FLIF16RangesContext *ranges_ctx,
                                FLIF16ColorVal *properties, uint8_t p,
                                uint32_t fr, uint32_t r, FLIF16ColorVal gray,
                                FLIF16ColorVal minP)
{
    // TODO write in a packet size independent manner
    // FLIF16ColorVal s->min = 0, s->max = 0;
    FLIF16ColorVal curr;
    uint32_t begin = 0, end = s->width;
    switch (s->segment2) {
        case 0:
            // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            // if this is a duplicate frame, copy the row from the frame being duplicated
            // TODO add this condition in read_ni_image
            if (s->frames[fr].seen_before >= 0) {
                //ff_flif16_copy_rows(CTX_CAST(s), &s->frames[fr], &s->frames[s->frames[fr].seen_before], p, r, 0, s->width);
                return 0;
            }

            // if this is not the first or only frame, fill the beginning of the row
            // before the actual pixel data
            // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            if (fr > 0) {
                //if alphazero is on, fill with a predicted value, otherwise
                // copy pixels from the previous frame
                // begin = image.col_begin[r];
                // end = image.col_end[r];
                begin = (!s->frameshape) ? 0 : s->frames[fr].col_begin[r];
                end = (!s->frameshape) ? s->width : s->frames[fr].col_end[r];
                if (s->alphazero && p < 3) {
                    for (uint32_t c = 0; c < begin; c++)
                        if (PIXEL_GET(s, fr, 3, r, c) == 0) {
                            printf("e %d %d %d\n", r, c, gray);
                            PIXEL_SET(s, fr, p, r, c, flif16_ni_predict(s, &s->frames[fr], p, r, c, gray));
                        } else {
                            printf("f %d %d %d %d\n", p, fr - 1, r, c);
                            PIXEL_SET(s, fr, p, r, c, PIXEL_GET(s, PREV_FRAMENUM(s->frames, fr), p, r, c));
                        }
                } else if (p != 4) {
                    ff_flif16_copy_rows(CTX_CAST(s), &s->frames[fr],
                                        PREV_FRAME(s->frames, fr), p, r, 0, begin);
                }
            }
            ++s->segment2;

            //printf("r = %u lookback = %d begin = %u end = %u\n", r, s->framelookback, begin, end);
            if (r > 1 && !s->framelookback && begin == 0 && end > 3) {
            // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            //decode actual pixel data
            s->c = begin;
            // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            
            for (; s->c < 2; s->c++) {
                if (s->alphazero && p<3 &&
                    PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                    //printf("<aa> 1\n");
                    PIXEL_SET(s, fr, p, r, s->c,
                    flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                    continue;
                }
                //printf("<a> 1\n");
                // printf("%d %d %d %d %d %d\n", p, r, s->c, s->min, s->max, minP);
                s->guess = flif16_ni_predict_calcprops(s, &s->frames[fr],
                           properties, ranges_ctx, p, r, s->c, &s->min, &s->max, minP, 0);
                // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
        case 1:
                // FLIF16ColorVal curr = coder.read_int(properties, s->min - s->guess, s->max - s->guess) + s->guess;
                //printf("<a> 2\n");
                MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                //printf("guess: %d curr: %d\n", s->guess, curr);
                //printf("%d %d %d", p, r, s->c);
                ff_flif16_pixel_set(CTX_CAST(s), &s->frames[fr], p, r, s->c, curr);
            }
            ++s->segment2;

            // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            for (; s->c < end-1; s->c++) {
                if (s->alphazero && p < 3 &&
                    ff_flif16_pixel_get(CTX_CAST(s), &s->frames[fr], 3, r, s->c) == 0) {
                    //printf("<aa> 2\n");
                    ff_flif16_pixel_set(CTX_CAST(s),&s->frames[fr], p, r, s->c,
                    flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                    continue;
                }
                //printf("<a> 3\n");
                // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                // printf("%d %d %d %d %d %d\n", p, r, s->c, s->min, s->max, minP);
                s->guess = flif16_ni_predict_calcprops(s, &s->frames[fr],
                           properties, ranges_ctx, p, r, s->c, &s->min, &s->max, minP, 1);
        case 2:
                // FLIF16ColorVal curr = coder.read_int(properties, s->min - s->guess, s->max - s->guess) + s->guess;
                //printf("<a> 4\n");
                MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                //printf("guess: %d curr: %d\n", s->guess, curr);
                //printf("%d %d %d", p, r, s->c);
                PIXEL_SET(s, fr, p, r, s->c, curr);
                __SUBST__
            }
            ++s->segment2;

            // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
            for (; s->c < end; s->c++) {
                if (s->alphazero && p < 3 &&
                    PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                    //printf("<aa> 3\n");
                    PIXEL_SET(s, fr, p, r, s->c, flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                    continue;
                }
               //printf("<a> 5\n");
               // printf("%d %d %d %d %d %d\n", p, r, s->c, s->min, s->max, minP);
               s->guess = flif16_ni_predict_calcprops(s, &s->frames[fr],
                          properties, ranges_ctx, p, r, s->c, &s->min, &s->max, minP, 0);
        case 3:
                // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                // FLIF16ColorVal curr = coder.read_int(properties, s->min - s->guess, s->max - s->guess) + s->guess;
                //printf("<a> 6\n");
                MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                //printf("guess: %d curr: %d\n", s->guess, curr);
                //printf("%d %d %d", p, r, s->c);
                PIXEL_SET(s, fr, p, r, s->c, curr);
                __SUBST__
            }
            ++s->segment2;

            } else {
                s->segment2 = 4;
                for (s->c = begin; s->c < end; s->c++) {
                    // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                    //predict pixel for alphazero and get a previous pixel for lookback
                    // printf("<><><><>%d %d %d\n",  s->alphazero, p, ff_flif16_pixel_get(CTX_CAST(s), &s->frames[fr], 3, r, s->c));
                    if (s->alphazero && p < 3 &&
                        ff_flif16_pixel_get(CTX_CAST(s), &s->frames[fr], 3, r, s->c) == 0) {
                        //printf("<<>> 1\n");
                        PIXEL_SET(s, fr, p, r, s->c,
                        flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                        continue;
                    }
                    if (s->framelookback && p < 4 &&
                        PIXEL_GET(s, fr, FLIF16_PLANE_LOOKBACK, r, s->c) > 0) {
                        //printf("<<>> 2\n");
                        // TODO accomodate PRE_FRAME for this
                        PIXEL_SET(s, fr, p, r, s->c,
                        PIXEL_GET(s, fr - PIXEL_GET(s, fr, FLIF16_PLANE_LOOKBACK, r, s->c), p, r, s->c));
                        continue;
                    }
                    //calculate properties and use them to decode the next pixel
                    //printf("<> 1\n");
                    // printf("%d %d %d %d %d %d\n", p, r, s->c, s->min, s->max, minP);
                    s->guess = flif16_ni_predict_calcprops(s, &s->frames[fr], properties,
                                                           ranges_ctx, p, r, s->c, &s->min,
                                                           &s->max, minP, 0);
                    if (s->framelookback && p == FLIF16_PLANE_LOOKBACK && s->max > fr)
                        s->max = fr;
        case 4:
                    //printf("<> 2\n");
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p,
                               s->min - s->guess, s->max - s->guess, &curr);
                    curr += s->guess;
                    //printf("guess: %d curr: %d\n", s->guess, curr);
                    //printf("%d %d %d", p, r, s->c);
                    PIXEL_SET(s, fr, p, r, s->c, curr);
                    __SUBST__
                }
            } /* end if */

            // If this is not the first or only frame, fill the end of the row after the actual pixel data
            if (fr > 0) {
                // printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                //if alphazero is on, fill with a predicted value, otherwise copy pixels from the previous frame
                if (s->alphazero && p < 3) {
                    for (uint32_t c = end; c < s->width; c++)
                        if (PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                            printf("ee %d %d %d\n", r, c, gray);
                            PIXEL_SET(s, fr,  p, r, s->c, flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                        } else {
                            printf("ff %d %d %d %d\n", p, PREV_FRAMENUM(s->frames, fr), r, c);
                            PIXEL_SET(s, fr, p, r, s->c, PIXEL_GET(s, PREV_FRAMENUM(s->frames, fr), p, r, s->c));
                        }
                } else if(p != 4) {
                    printf("gg %d %d %d %d\n", p, PREV_FRAMENUM(s->frames, fr), r, begin);
                     ff_flif16_copy_rows(CTX_CAST(s), &s->frames[fr],
                     PREV_FRAME(s->frames, fr), p, r, end, s->width);
                }
            }
    }

    s->segment2 = 0;
    return 0;

    need_more_data:
    //printf(">>>> Need more data\n");
    return AVERROR(EAGAIN);
}


static FLIF16ColorVal *compute_grays(FLIF16RangesContext *ranges)
{
    FLIF16ColorVal *grays; // a pixel with values in the middle of the bounds
    grays = av_malloc(ranges->num_planes * sizeof(*grays));
    if (!grays)
        return NULL;
    for (int p = 0; p < ranges->num_planes; p++)
        grays[p] = (ff_flif16_ranges_min(ranges, p) + ff_flif16_ranges_max(ranges, p)) / 2;
    return grays;
}

static int flif16_read_ni_image(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    int ret;
    FLIF16ColorVal min_p;
    /*
    for (int p = 0; p < images[0].numPlanes(); p++) {
        Ranges propRanges;
        initPropRanges_scanlines(propRanges, *ranges, p);
        coders.emplace_back(rac, propRanges, forest[p], 0, options.cutoff, options.alpha);
    }*/

    // The FinalPropertySymbolCoder does not use the propranges at any point of time.
    // Only the size of propranges is used, which can by calculated in a single
    // line copypasted from flif16.c. Not even that is necessary. Therefore this
    // is completely useless.

    // To read MANIAC integers, do:
    // ff_flif16_maniac_read_int(s->rc, s->maniac_ctx, properties, plane, min, max, &target)
    // Or something like that. Check out the definition in flif16_rangecoder.c

    // Set images to gray
    switch (s->segment) {
        case 0:
            s->grays = compute_grays(s->range); // free later
            if (!s->grays)
                return AVERROR(ENOMEM);
            s->i = s->i2 = s->i3 = 0;
            if (   (s->range->num_planes > 3 && ff_flif16_ranges_max(s->range, 3) == 0)
                || (s->range->num_planes > 3 && ff_flif16_ranges_min(s->range, 3) > 0))
                s->alphazero = 0;
            
            ++s->segment;
            
            for (; s->i < 5; ++s->i) {
                s->curr_plane = plane_ordering[s->i];
                if (s->curr_plane >= s->num_planes) {
                    continue;
                }
                // printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                if (ff_flif16_ranges_min(s->range, s->curr_plane) >=
                    ff_flif16_ranges_max(s->range, s->curr_plane)) {
                    continue;
                }
                // printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                s->properties = av_mallocz((s->num_planes > 3 ? properties_ni_rgba_size[s->curr_plane]
                                                            : properties_ni_rgb_size[s->curr_plane]) 
                                                            * sizeof(*s->properties));
                if (!s->properties)
                    return AVERROR(ENOMEM);
                // printf("sizeof s->properties: %u\n", (s->num_planes > 3 ? properties_ni_rgba_size[s->curr_plane]:
                //                                                        properties_ni_rgb_size[s->curr_plane]));
                // printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                for (; s->i2 < s->height; ++s->i2) {
                    for (; s->i3 < s->num_frames; ++s->i3) {
        case 1:
                        // printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                        // TODO maybe put this in dec ctx
                        min_p = ff_flif16_ranges_min(s->range, s->curr_plane);
                        ret = flif16_read_ni_plane(s, s->range, s->properties,
                                                   s->curr_plane,
                                                   s->i3,
                                                   s->i2,
                                                   s->grays[s->curr_plane],
                                                   min_p);
                        
                        if (ret) {
                            printf("Caught Ret: %u\n", ret);
                            goto error;
                        }
                    } // End for
                    s->i3 = 0;
                } // End for
                if (s->properties)
                    av_freep(&s->properties);
                s->i2 = 0;
            } // End for
            
    } // End switch

    if (s->grays)
            av_freep(&s->grays);

    for (int k = 0; k < s->num_planes; ++k) {
        for (int j = 0; j < s->height; ++j) {
            for (int i = 0; i < s->width; ++i) {
                printf("%d ", ff_flif16_pixel_get(CTX_CAST(s), &s->frames[0], k, j, i));
            }
            printf("\n");
        }
        printf("===\n");
    }

    for (int i = 0; i < s->num_frames; i++) {
        if (s->frames[i].seen_before >= 0)
            continue;
        for (int j = s->transform_top - 1; j >= 0; --j) {
            ff_flif16_transform_reverse(CTX_CAST(s), s->transforms[j], &s->frames[i], 1, 1);
            printf("Transform Step %d\n===========\n", s->transforms[j]->t_no);
            for (int k = 0; k < s->num_planes; ++k) {
                for (int j = 0; j < s->height; ++j) {
                    for (int i = 0; i < s->width; ++i) {
                        printf("%d ", ff_flif16_pixel_get(CTX_CAST(s), &s->frames[0], k, j, i));
                    }
                    printf("\n");
                }
                printf("===\n");
            }
        }
    }
    s->state = FLIF16_OUTPUT;
    return 0;

    error:
    printf("<><><> Error return.\n");
    return ret;
}

/* ============================================================================
 * Interlaced plane decoding
 * ============================================================================
 *
 * This is how the data is organised here:
 * 1. uni_int: rough_zoomlevel
 * 2. (repeat num_planes times) values of top left pixels of each channel
 * 3. Rough Pixeldata max_zoomlevel to rough_zoomlevel + 1
 *    For this case, the MANIAC forest is initialised with a single node per
 *    channel. This is nused with the maniac integer reader.
 * 4. Actual Encoded MANIAC trees
 * 5. Rest of the pixeldata rough_zoomlevel to 0
 * 
 */
#if 1
// TODO combine these 2 funcs
static inline FLIF16ColorVal flif16_predict_horizontal(FLIF16Context *s, FLIF16PixelData *frame,
                                                       int z, int p, uint32_t r, uint32_t c,
                                                       uint32_t rows, const int predictor)
{
    FLIF16ColorVal top, bottom, avg, left, topleft, bottomleft;
    if (p == FLIF16_PLANE_LOOKBACK)
        return 0;
    // assert(z%2 == 0); // filling horizontal lines
    top    = ff_flif16_pixel_getz(s, frame, p, z, r - 1, c);
    bottom = (r+1 < rows ? ff_flif16_pixel_getz(s, frame, p, z,r+1,c) : top ); // (c > 0 ? image(p, z, r, c - 1) : top));
    if (predictor == 0) {
        avg = (top + bottom)>>1;
        return avg;
    } else if (predictor == 1) {
        avg        = (top + bottom)>>1;
        left       = (c>0 ? ff_flif16_pixel_getz(s, frame, p, z,r,c-1) : top);
        topleft    = (c>0 ? ff_flif16_pixel_getz(s, frame, p, z,r-1,c-1) : top);
        bottomleft = (c>0 && r+1 < rows ? ff_flif16_pixel_getz(s, frame, p, z,r+1,c-1) : left);
        return MEDIAN3(avg, (FLIF16ColorVal)(left+top-topleft), (FLIF16ColorVal)(left+bottom-bottomleft));
    } else { // if (predictor == 2) {
        left = (c>0 ? ff_flif16_pixel_getz(s, frame, p, z,r,c-1) : top);
        return MEDIAN3(top,bottom,left);
    }
}

static inline FLIF16ColorVal flif16_predict_vertical(FLIF16Context *s, FLIF16PixelData *frame,
                                                     int z, int p, uint32_t r, uint32_t c,
                                                     uint32_t cols, const int predictor)
{
    FLIF16ColorVal top, left, right, avg, topleft, topright;
    if (p == FLIF16_PLANE_LOOKBACK)
        return 0;
    //assert(z%2 == 1); // filling vertical lines
    left  = ff_flif16_pixel_getz(s, frame, p, z,r,c-1);
    right = (c+1 < cols ? ff_flif16_pixel_getz(s, frame, p, z, r, c + 1) : left ); //(r > 0 ? image(p, z, r-1, c) : left));
    if (predictor == 0) {
        avg = (left + right)>>1;
        return avg;
    } else if (predictor == 1) {
        avg      = (left + right)>>1;
        top      = (r>0 ? ff_flif16_pixel_getz(s, frame, p, z, r - 1, c) : left);
        topleft  = (r>0 ? ff_flif16_pixel_getz(s, frame, p, z , r - 1, c - 1) : left);
        topright = (r>0 && c+1 < cols ? ff_flif16_pixel_getz(s, frame, p, z,r-1,c+1) : top);
        return MEDIAN3(avg, (FLIF16ColorVal)(left+top-topleft), (FLIF16ColorVal)(right+top-topright));
    } else { // if (predictor == 2) {
        top = (r>0 ? ff_flif16_pixel_getz(s, frame, p, z, r - 1, c) : left);
        return MEDIAN3(top,left,right);
    }
}

static inline FLIF16ColorVal flif16_predict(FLIF16Context *s, FLIF16PixelData *frame,
                                            int z, int p, uint32_t r, uint32_t c,
                                            const int predictor)
{
    FLIF16ColorVal prediction;
    if (p == FLIF16_PLANE_LOOKBACK)
        return 0;
    if (z%2 == 0) { // filling horizontal lines
        prediction = flif16_predict_horizontal(s, frame,z,p,r,c,ZOOM_HEIGHT(s->height, z),predictor);
    } else { // filling vertical lines
        prediction = flif16_predict_vertical(s, frame,z,p,r,c,ZOOM_WIDTH(s->width, z),predictor);
    }

    // accurate snap-to-ranges: too expensive?
    //    if (p == 1 || p == 2) {
    //      prevPlanes pp(p);
    //      ColorVal min, max;
    //      for (int prev=0; prev<p; prev++) pp[prev]=image(prev,z,r,c);
    //      ranges->snap(p,pp,min,max,prediction);
    //    }
    return prediction;

}

static FLIF16ColorVal flif16_predict_calcprops(FLIF16Context *s,
                                               FLIF16PixelData *frame,
                                               FLIF16ColorVal *properties,
                                               FLIF16RangesContext *ranges,
                                               int z, uint8_t p, uint32_t r,
                                               uint32_t c, FLIF16ColorVal *min,
                                               FLIF16ColorVal *max,
                                               int predictor, uint8_t horizontal,
                                               uint8_t nobordercases)
{
    FLIF16ColorVal guess, left, top, topleft, topright, bottomleft, bottom,
                   avg, topleftgradient, median, bottomright, right;
    const uint8_t bottomPresent = r + 1 < ZOOM_HEIGHT(s->height, z);
    const uint8_t rightPresent  = c + 1 < ZOOM_WIDTH(s->width, z);
    int which = 0;
    int index = 0;

    ff_flif16_prepare_zoomlevel(s, frame, 0, z);
    ff_flif16_prepare_zoomlevel(s, frame, p, z);

    if (p < 3) {
        if (p > 0)
            properties[index++] = PIXELY(z,r,c);
        if (p > 1)
            properties[index++] = ff_flif16_pixel_getz(s, frame, FLIF16_PLANE_CO, z, r, c);
        if (s->num_planes > 3)
            properties[index++] = ff_flif16_pixel_getz(s, frame, FLIF16_PLANE_ALPHA, z, r, c);
    }

    if (horizontal) { // filling horizontal lines
        top = PIXEL(z,r-1,c);
        left =  (nobordercases || c > 0 ? PIXEL(z, r, c - 1) : top);
        topleft = (nobordercases || c > 0 ? PIXEL(z, r - 1, c - 1) : top);
        topright = (nobordercases || (rightPresent) ? PIXEL(z, r - 1, c + 1) : top);
        bottomleft = (nobordercases || (bottomPresent && c > 0) ? PIXEL(z,r + 1, c - 1) : left);
        bottom          = (nobordercases || bottomPresent ? PIXEL(z,r + 1, c) : left);
        avg             = (top + bottom) >> 1;
        topleftgradient = left + top - topleft;
        median          = MEDIAN3(avg, topleftgradient, (FLIF16ColorVal)(left+bottom-bottomleft));
        which = 2;
        if (median == avg)
            which = 0;
        else if (median == topleftgradient)
            which = 1;
        properties[index++] = which;
        if (p == FLIF16_PLANE_CO || p == FLIF16_PLANE_CG) {
            properties[index++] = PIXELY(z,r,c) - ((PIXELY(z, r - 1, c) +
                                  PIXELY(z,(nobordercases || bottomPresent ? r + 1 : r - 1), c)) >> 1);
        }
        if (predictor == 0) guess = avg;
        else if (predictor == 1)
            guess = median;
        else //if (predictor == 2)
            guess = MEDIAN3(top,bottom,left);
        ff_flif16_ranges_snap(ranges, p, properties, min, max, &guess);
        properties[index++] = top-bottom;
        properties[index++] = top-((topleft+topright)>>1);
        properties[index++] = left-((bottomleft+topleft)>>1);
        bottomright = (nobordercases || (rightPresent && bottomPresent) ? PIXEL(z,r+1,c+1) : bottom);
        properties[index++] = bottom-((bottomleft+bottomright)>>1);
    } else { // filling vertical lines
        left = PIXEL(z,r,c-1);
        top = (nobordercases || r>0 ? PIXEL(z,r-1,c) : left);
        topleft = (nobordercases || r>0 ? PIXEL(z,r-1,c-1) : left);
        topright = (nobordercases || (r>0 && rightPresent) ? PIXEL(z,r-1,c+1) : top);
        bottomleft = (nobordercases || (bottomPresent) ? PIXEL(z,r+1,c-1) : left);
        right = (nobordercases || rightPresent ? PIXEL(z,r,c+1) : top);
        avg = (left + right)>>1;
        topleftgradient = left+top-topleft;
        median = MEDIAN3(avg, topleftgradient, (FLIF16ColorVal) (right+top-topright));
        which = 2;
        if (median == avg)
            which = 0;
        else if (median == topleftgradient)
            which = 1;
        properties[index++] = which;
        if (p == FLIF16_PLANE_CO || p == FLIF16_PLANE_CG) {
            properties[index++] = PIXELY(z,r,c) - ((PIXELY(z,r,c-1)+PIXELY(z,r,(nobordercases || rightPresent ? c+1 : c-1)))>>1);
        }
        if (predictor == 0)
            guess = avg;
        else if (predictor == 1)
            guess = median;
        else //if (predictor == 2)
            guess = MEDIAN3(top, left, right);
        ff_flif16_ranges_snap(ranges, p, properties, min, max, &guess);
        properties[index++] = left - right;
        properties[index++] = left -((bottomleft + topleft) >> 1);
        properties[index++] = top - ((topleft + topright) >> 1);
        bottomright = (nobordercases || (rightPresent && bottomPresent) ? PIXEL(z, r + 1, c + 1) : right);
        properties[index++] = right-((bottomright + topright) >> 1);
    }
    properties[index++] = guess;
//    if (p < 1 || p > 2) properties[index++]=which;


//    properties[index++]=left - topleft;
//    properties[index++]=topleft - top;

//    if (p == 0 || p > 2) {
//        if (nobordercases || (c+1 < image.cols(z) && r > 0)) properties[index++]=top - topright;
//        else properties[index++]=0;
//    }
    if (p != 2) {
        if (nobordercases || r > 1)
            properties[index++] = PIXEL(z, r - 2, c) - top;    // toptop - top
        else
            properties[index++] = 0;
        if (nobordercases || c > 1)
            properties[index++] = PIXEL(z, r, c - 2) - left;    // leftleft - left
        else
            properties[index++] = 0;
    }
    return guess;
}

static inline int plane_zoomlevels(uint8_t num_planes, int begin_zl, int end_zl)
{
    return num_planes * (begin_zl - end_zl + 1);
}

static void plane_zoomlevel(uint8_t num_planes, int begin_zl, int end_zl,
                            int i, FLIF16RangesContext *ranges, int *min, int *max)
{
    int zl_list[MAX_PLANES] = {0};
    int nextp, p, zl, highest_priority_plane;

    // av_assert0(i >= 0);
    // av_assert0(i < plane_zoomlevels(image, beginZL, endZL));
    // simple order: interleave planes, zoom in
//    int p = i % image.numPlanes();
//    int zl = beginZL - (i / image.numPlanes());

    // more advanced order: give priority to more important plane(s)
    // assumption: plane 0 is luma, plane 1 is chroma, plane 2 is less important
    // chroma, plane 3 is perhaps alpha, plane 4 are frame lookbacks (lookback transform, animation only)
    int max_behind[] = {0, 2, 4, 0, 0};

    // if there is no info in the luma plane, there's no reason to lag chroma behind luma
    // (this also happens for palette images)
    if (ff_flif16_ranges_min(ranges, 0) >= ff_flif16_ranges_max(ranges, 0)) {
        max_behind[1] = 0;
        max_behind[2] = 1;
    }

    if (num_planes > 5) {
        // too many planes, do something simple
        p = i % num_planes;
        zl = begin_zl - (i / num_planes);
        *min = p;
        *max = zl;
        return;
    }

    for (int i = 0; i < num_planes; ++i)
        zl_list[i] = begin_zl + 1;
    highest_priority_plane = 0;
    if (num_planes >= 4)
        highest_priority_plane = 3; // alpha first
    if (num_planes >= 5)
        highest_priority_plane = 4; // lookbacks first
    nextp = highest_priority_plane;

    while (i >= 0) {
        --zl_list[nextp];
        --i;
        if (i < 0)
            break;
        nextp = highest_priority_plane;
        for (int p = 0; p < num_planes; p++) {
            if (zl_list[p] > zl_list[highest_priority_plane] + max_behind[p]) {
                nextp = p; //break;
            }
        }
        // ensure that nextp is not at the most detailed zoomlevel yet
        while (zl_list[nextp] <= end_zl)
            nextp = (nextp + 1) % num_planes;
    }
    p = nextp;
    zl = zl_list[p];

    *min = p;
    *max = zl;
}

// specialized decode functions (for speed)
// assumption: plane and alpha are prepare_zoomlevel(z)
static int  flif_decode_plane_zoomlevel_horizontal(FLIF16DecoderContext *s,
                                                   FLIF16ColorVal *properties,
                                                   FLIF16RangesContext *ranges,
                                                   uint8_t alpha_plane,
                                                   int p, int z, uint32_t fr, uint32_t r,
                                                   uint8_t alphazero, uint8_t lookback,
                                                   int predictor, int invisible_predictor)
                                            
{
    FLIF16ColorVal min, max, guess, curr;
    uint32_t begin = 0, end = ZOOM_WIDTH(s->width, z);
    switch (s->segment2) {
        case 0:
            if (s->frames[fr].seen_before >= 0) {
                uint32_t cs = ZOOM_COLPIXELSIZE(z); // getscale used here
                uint32_t rs = ZOOM_ROWPIXELSIZE(z); // getscale used here
                ff_flif16_copy_rows_stride(CTX_CAST(s), &s->frames[fr],
                                           &s->frames[s->frames[fr].seen_before],
                                           p , rs * r, 0, cs * ZOOM_WIDTH(s->width, z), cs);
                return 0;
            }
            if (fr > 0) {
                // Replace entrirely?
                begin = (0)/ZOOM_COLPIXELSIZE(z);
                end = 1 + (s->width - 1)/ZOOM_COLPIXELSIZE(z);
                if (s->alphazero && p < 3) {
                    for (s->c = 0; s->c < begin; ++s->c)
                        if (PIXEL_GETZ(s, fr, FLIF16_PLANE_ALPHA, z, r, s->c) == 0)
                            PIXEL_SETZ(s, fr, p,  z, r, s->c,
                            flif16_predict_horizontal(CTX_CAST(s), &s->frames[fr], p, z, r, s->c, ZOOM_HEIGHT(s->height, z), invisible_predictor));
                        else
                            PIXEL_SETZ(s, fr, p, z, r, s->c, PIXEL_GETZ(s, fr - 1, p, z, r, s->c));
        // have to wait with the end of the row until the part between begin and end is decoded
        //            for (uint32_t c = end; c < ZOOM_WIDTH(s->width, z); c++)
        //                if (alpha.get(z,r,c) == 0) p.set(z,r,c, predict_p_horizontal(p,z,p,r,c, ZOOM_HEIGHT(s->height, z), invisible_predictor));
        //                else image.set(p,z,r,c,images[fr-1](p,z,r,c));
                } else if (p != 4) {
                    const uint32_t cs = ZOOM_COLPIXELSIZE(z), rs = ZOOM_ROWPIXELSIZE(z);
                    ff_flif16_copy_rows_stride(CTX_CAST(s), &s->frames[fr],
                                               &s->frames[fr - 1], p,
                                               rs*r, cs*0, cs*begin, cs);
                    ff_flif16_copy_rows_stride(CTX_CAST(s), &s->frames[fr],
                                               &s->frames[fr - 1], p,
                                               rs*r, cs*end,
                                               cs*ZOOM_WIDTH(s->width, z), cs);
                }
            }

            // avoid branching for border cases
            if (r > 1 && r < ZOOM_HEIGHT(s->height, z)-1 && !lookback && begin == 0 && end > 3) {
                for (s->c = begin; s->c < 2; ++s->c) {
                    // TODO change FLIF16_PLANE_ALPHA to variable as appropriate
                    if (alphazero && p < 3 && PIXEL_GETFAST(s, fr, FLIF16_PLANE_ALPHA, r, s->c) == 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c, flif16_predict_horizontal(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_HEIGHT(s->height, z), invisible_predictor));
                        continue;
                    }
                    //guess = flif16_predict_calcprops(properties,ranges,image,p,pY,z,r,c,min,max, predictor);
                    guess = flif16_predict_calcprops(CTX_CAST(s), &s->frames[fr], properties, ranges, z, p, r, s->c, &min, &max, predictor, 1, 0);
        case 1:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p, min - guess, max - guess, &curr);
                    curr += guess;
                    PIXEL_SETFAST(s, fr, p, r, s->c, curr);
                }
                for (s->c = 2; s->c < end-2; s->c++) {
                    if (alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c, flif16_predict_horizontal(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_HEIGHT(s->height, z), invisible_predictor));
                        continue;
                    }
                    //\guess = predict_and_calcProps_p<p_t,alpha_t,true,true,p,ranges_t>(properties,ranges,image,p,pY,z,r,c,min,max, predictor);
                    guess = flif16_predict_calcprops(CTX_CAST(s), &s->frames[fr], properties, ranges, z, p, r, s->c, &min, &max, predictor, 1, 1);
        case 2:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p, min - guess, max - guess, &curr);
                    curr += guess;
                    PIXEL_SETFAST(s, fr, p, r, s->c, curr);
                }
                for (s->c = end - 2; s->c < end; s->c++) {
                    if (alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c, flif16_predict_horizontal(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_HEIGHT(s->height, z), invisible_predictor));
                        continue;
                    }
                    //\guess = predict_and_calcProps_p<p_t,alpha_t,true,false,p,ranges_t>(properties,ranges,image,p,pY,z,r,c,min,max, predictor);
                    guess = flif16_predict_calcprops(CTX_CAST(s), &s->frames[fr], properties, ranges, z, p, r, s->c, &min, &max, predictor, 1, 0);
        case 3:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p, min - guess, max - guess, &curr);
                    curr += guess;
                    PIXEL_SETFAST(s, fr, p, r, s->c, curr);
                }
            } else {
                for (uint32_t c = begin; c < end; c++) {
                    if (alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c, flif16_predict_horizontal(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_HEIGHT(s->height, z), invisible_predictor));
                        continue;
                    }
                    if (lookback && p<4 && PIXEL_GETZ(s, fr, FLIF16_PLANE_LOOKBACK, z,r,c) > 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c, PIXEL_GETZ(s, fr - PIXEL_GETZ(s, fr, FLIF16_PLANE_LOOKBACK, z, r, s->c), p, z, r, s->c));
                        continue;
                    }
                    //guess = predict_and_calcProps_p<p_t,alpha_t,true,false,p,ranges_t>(properties,rcanges,image,p,pY,z,r,c,min,max, predictor);
                    guess = flif16_predict_calcprops(CTX_CAST(s), &s->frames[fr], properties, ranges, z, p, r, s->c, &min, &max, predictor, 1, 0);

                    if (s->framelookback && p == FLIF16_PLANE_LOOKBACK && max > fr)
                        max = fr;
                    if (s->framelookback && (guess > max || guess < min))
                        guess = min;
        case 4:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p, min - guess, max - guess, &curr);
                    curr += guess;
                    //assert(curr >= ranges->min(p) && curr <= ranges->max(p));
                    //assert(curr >= min && curr <= max);
                    PIXEL_SETFAST(s, fr, p, r,c, curr);
                }
            }

            if (fr>0 && alphazero && p < 3) { // Deal with the alpha flag
                for (uint32_t c = end; c < ZOOM_WIDTH(s->width, z); c++)
                    if (PIXEL_GETZ(s, fr, p, z, r, s->c) == 0)
                        PIXEL_SETZ(s, fr, p, z, r, s->c, flif16_predict_horizontal(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_HEIGHT(s->height, z), invisible_predictor));
                    else
                        PIXEL_SETZ(s, fr, p, z, r, s->c, PIXEL_GETZ(s, fr - 1, p, z, r, s->c));
            }
    }

    return 0;

    need_more_data:
    return AVERROR(EAGAIN);
}

static int flif16_decode_plane_zoomlevel_vertical(FLIF16DecoderContext *s,
                                                  FLIF16ColorVal *properties,
                                                  FLIF16RangesContext *ranges,
                                                  uint8_t alpha_plane,
                                                  int p, int z, uint32_t fr, uint32_t r,
                                                  uint8_t alphazero, uint8_t lookback,
                                                  int predictor, int invisible_predictor)
{
    FLIF16ColorVal min,max, guess, curr;
    uint32_t begin = 1, end = ZOOM_WIDTH(s->width, z);

    switch (s->segment2) {
        case 0:
            if (s->frames[fr].seen_before >= 0) {
                const uint32_t cs = ZOOM_COLPIXELSIZE(z);
                const uint32_t rs = ZOOM_ROWPIXELSIZE(z);
                //\copy_row_range(p, images[image.seen_before].getPlane(p),rs*r,cs*1,cs*ZOOM_WIDTH(s->width, z),cs*2);
                ff_flif16_copy_rows_stride(CTX_CAST(s), &s->frames[fr],
                                           &s->frames[s->frames[fr].seen_before],
                                           p , rs * r, cs*1, cs * ZOOM_WIDTH(s->width, z), cs*2);
                return 0;
            }
            if (fr > 0) {
                //\begin = (image.col_begin[r * ZOOM_ROWPIXELSIZE(z)]/ZOOM_COLPIXELSIZE(z));
                //\end = (1 + (image.col_end[r * ZOOM_ROWPIXELSIZE(z)] - 1)/ ZOOM_COLPIXELSIZE(z))) | 1;
                begin = 0;
                end = s->width | 1; // ???
                if (begin > 1 && ((begin & 1) == 0))
                    --begin;
                if (begin == 0)
                    begin=1;
                if (alphazero && p < 3) {
                    for (s->c = 1; s->c < begin; s->c += 2)
                        if (PIXEL_GETZ(s, fr, alpha_plane, z, r, s->c) == 0)
                            PIXEL_SETZ(s, fr, p, z, r, s->c,flif16_predict_vertical(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_WIDTH(s->width, z), invisible_predictor));
                        else
                            PIXEL_SETZ(s, fr, p, z, r, s->c, PIXEL_GETZ(s, fr - 1, p, z, r, s->c));
        // have to wait with the end of the row until the part between begin and end is decoded
        //            for (uint32_t c = end; c < ZOOM_WIDTH(s->width, z); c += 2)
        //                if (alpha.get(z, r, s->c) == 0) p.set(z, r, s->c, predict_p_vertical(p, z, p, r, s->c, ZOOM_WIDTH(s->width, z), invisible_predictor));
        //                else image.set(p,z,r,s->c,images[fr-1](p,z,r,s->c));
                } else if (p != 4) {
                    const uint32_t cs = ZOOM_COLPIXELSIZE(z);
                    const uint32_t rs = ZOOM_ROWPIXELSIZE(z);
                    //\ff_flif16_copy_rows(s, s, (p, images[fr - 1].getPlane(p), rs*r, cs*1, cs*begin, cs*2);
                    ff_flif16_copy_rows_stride(CTX_CAST(s), &s->frames[fr],
                                               &s->frames[fr - 1], p,
                                               rs*r, cs*1, cs*begin, cs*2);
                    //\copy_row_range(p, images[fr - 1].getPlane(p), rs*r, cs*end, cs*ZOOM_WIDTH(s->width, z), cs*2);
                    ff_flif16_copy_rows_stride(CTX_CAST(s), &s->frames[fr],
                                               &s->frames[fr - 1], p,
                                               rs*r, cs*end, cs*ZOOM_WIDTH(s->width, z), cs*2);
                    
                }
            }
            // avoid branching for border cases
            if (r > 1 && r < ZOOM_HEIGHT(s->height, z)-1 && !lookback && end == ZOOM_WIDTH(s->width, z) && end > 5 && begin == 1) {
                s->c = begin;
                for (; s->c < 3; s->c += 2) {
                    if (alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r,s->c) == 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c, flif16_predict_vertical(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_WIDTH(s->width, z), invisible_predictor));
                        continue;
                    }
                    //\guess = predict_and_calcProps_p<p_t,alpha_t,false,false,p,ranges_t>(properties,ranges,image,p,pY,z,r,s->c,min,max, predictor);
                    guess =flif16_predict_calcprops(CTX_CAST(s), &s->frames[fr], properties, ranges, z, p, r, s->c, &min, &max, predictor, 0, 0);
        case 1:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p, min - guess, max - guess, &curr);
                    curr += guess;
                    //\p.set_fast(r, s->c, curr);
                    PIXEL_SETFAST(s, fr, p, r, s->c, curr);
                }
                for (; s->c < end - 2; s->c += 2) {
                    if (alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c,flif16_predict_vertical(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_WIDTH(s->width, z), invisible_predictor));
                        continue;
                    }
                    //\guess = predict_and_calcProps_p<p_t,alpha_t,false,true,p,ranges_t>(properties,ranges,image,p,pY,z,r,s->c,min,max, predictor);
                    flif16_predict_calcprops(CTX_CAST(s), &s->frames[fr], properties, ranges, z, p, r, s->c, &min, &max, predictor, 0, 1);
        case 2:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p, min - guess, max - guess, &curr);
                    curr += guess;
                    //\p.set_fast(r,s->c, curr);
                    PIXEL_SETFAST(s, fr, p, r, s->c, curr);
                }
                for (; s->c < end; s->c += 2) {
                    if (alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c, flif16_predict_vertical(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_WIDTH(s->width, z), invisible_predictor));
                        continue;
                    }
                    //\guess = predict_and_calcProps_p<p_t,alpha_t,false,false,p,ranges_t>(properties,ranges,image,p,pY,z,r,s->c,min,max, predictor);
                    flif16_predict_calcprops(CTX_CAST(s), &s->frames[fr], properties, ranges, z, p, r, s->c, &min, &max, predictor, 0, 0);
        case 3:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p, min - guess, max - guess, &curr);
                    curr += guess;
                    //\p.set_fast(r, s->c, curr);
                    PIXEL_SETFAST(s, fr, p, r, s->c, curr);
                }
            } else {
                for (s->c = begin; s->c < end; s->c += 2) {
                    if (alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c,flif16_predict_vertical(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_WIDTH(s->width, z), invisible_predictor));
                        continue;
                    }
                    if (lookback && p < 4 && PIXEL_GETZ(s, fr, FLIF16_PLANE_LOOKBACK, z, r, s->c) > 0) {
                        PIXEL_SETFAST(s, fr, p, r, s->c, PIXEL_GETZ(s, fr - PIXEL_GETZ(s, fr, FLIF16_PLANE_LOOKBACK, z, r, s->c), p, z, r, s->c));
                        continue;
                    }
                    //\guess = predict_and_calcProps_p<p_t,alpha_t,false,false,p,ranges_t>(properties,ranges,image,p,pY,z,r,s->c,min,max, predictor);
                    flif16_predict_calcprops(CTX_CAST(s), &s->frames[fr], properties, ranges, z, p, r, s->c, &min, &max, predictor, 0, 0);
                    if (s->framelookback && p == FLIF16_PLANE_LOOKBACK && max > fr)
                        max = fr;
                    if (s->framelookback && (guess > max || guess < min))
                        guess = min;
        case 4:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p, min - guess, max - guess, &curr);
                    curr += guess;
                    
                    ////p.set(z,r,s->c, curr);
                    // assert(curr >= ranges->min(p) && curr <= ranges->max(p));
                    // assert(curr >= min && curr <= max);
                    //\p.set_fast(r, s->c, curr);
                    ff_flif16_pixel_set_fast(CTX_CAST(s), &s->frames[fr], p, r, s->c, curr);
                }
            }
    }

    if (fr > 0 && alphazero && p < 3) {
        for (s->c = end; s->c < ZOOM_WIDTH(s->width, z); s->c += 2)
            // replace enum with variable
            if (PIXEL_GETZ(s, fr - 1, alpha_plane, z, r, s->c) == 0)
                PIXEL_SETZ(s, fr, p, z, r, s->c, flif16_predict_vertical(CTX_CAST(s), &s->frames[fr], z, p, r, s->c, ZOOM_WIDTH(s->width, z), invisible_predictor));
            else
                PIXEL_SETZ(s, fr, p, z, r, s->c, PIXEL_GETZ(s, fr - 1, p, z, r, s->c));
    }


    s->segment2 = 0;
    return 0;

    need_more_data:
    return AVERROR(EAGAIN);

}

static int flif16_read_image(AVCodecContext *avctx, uint8_t rough) {
    FLIF16DecoderContext *s = avctx->priv_data;
    int ret;
    int temp;
    int minR;
    int begin_zl, end_zl; // TODO put in ctx
    uint8_t nump = s->num_planes;
    // Replace with ctx vars
    int quality = s->quality; // TODO put in ctx
    int scale = s->scale;// TODO put in ctx
    uint8_t default_order;// TODO put in ctx
    int p;// TODO put in ctx
    int zl_first, zl_second;// TODO put in ctx
    int z;// TODO put in ctx
    // TODO replace constant_alpha.  && s->frames[0].constant_alpha below
    uint8_t alpha_plane = (s->num_planes > 3) ? 3 : 0;
    int the_predictor[5] = {0};
    int predictor;
    int breakpoints = 0;  // TODO change

    if (!rough) {
        begin_zl = s->rough_zl;
        end_zl = 0;
        s->segment = 4;
    } else {
        if (!s->maniac_ctx.forest) {
            s->maniac_ctx.forest = av_mallocz((s->num_planes) * sizeof(*(s->maniac_ctx.forest)));
            if (!s->maniac_ctx.forest) {
                return AVERROR(ENOMEM);
            }
            for (int i = 0; i < s->num_planes; ++i) {
                s->maniac_ctx.forest[i]->data = av_mallocz(sizeof(*s->maniac_ctx.forest[i]->data));
                if (!s->maniac_ctx.forest[i]->data)
                    return AVERROR(ENOMEM);
                s->maniac_ctx.forest[i]->data[0].property = -1;
            }
        }
    }

    switch (s->segment) {
        case 0:
            s->zooms = 0;
            // get_rough_zl()
            while (    ZOOM_ROWPIXELSIZE(s->zooms) < s->height
                    || ZOOM_COLPIXELSIZE(s->zooms) < s->width)
                ++s->zooms;
            begin_zl = s->zooms; //TODO MANAGE
            ++s->segment;
            
        case 1:
            RAC_GET(&s->rc, NULL, 0, s->zooms, &s->rough_zl, FLIF16_RAC_UNI_INT32);
            end_zl = s->rough_zl; //TODO MANAGEs
            ++s->segment;

            // special case: very left top pixel must be read first to get it all started
            for (s->i = 0; s->i < s->num_planes; s->i++) {
                if (ff_flif16_ranges_min(s->range, s->i) < ff_flif16_ranges_max(s->range, s->i)) {
                     minR = ff_flif16_ranges_min(s->range, s->i);
        case 2:
                     RAC_GET(&s->rc, NULL, minR, ff_flif16_ranges_max(s->range, s->i) - minR,
                             &temp, FLIF16_RAC_UNI_INT32);
                     PIXEL_SETZ(s, 0, p, 0, 0, 0, temp);// TODO change?
                     // Add support for zoomlevel pixel set(ter).
                }
            }
            ++s->segment;

        case 3:
            s->zoomlevels = av_malloc(nump * sizeof(*s->zoomlevels));
            if (!s->zoomlevels)
                return AVERROR(ENOMEM);
            memset(s->zoomlevels, begin_zl, nump);
            ++s->segment;

        case 4:
            RAC_GET(&s->rc, NULL, 0, 1, &default_order, FLIF16_RAC_UNI_INT8);
            ++s->segment;

            for (s->i = 0; s->i < nump; s->i++) {
        case 5:
                RAC_GET(&s->rc, NULL, 0, MAX_PREDICTORS + 1, &s->predictors[p], FLIF16_RAC_UNI_INT32);
            }
            ++s->segment;

            for (s->i = 0; s->i < plane_zoomlevels(nump, begin_zl, end_zl); s->i++) {
                if (default_order) {
                    plane_zoomlevel(s->num_planes, begin_zl, end_zl, s->i, s->range, &zl_first, &zl_second);
                    p = zl_first;
                    // assert(zoomlevels[p] == pzl.second);
                } else {
                    // p = metaCoder.read_int(0, nump-1);
        case 6:
                    RAC_GET(&s->rc, NULL, 0, nump - 1, &p, FLIF16_RAC_UNI_INT32);
                    ++s->segment;
                    //if (nump > 3 && images[0].alpha_zero_special && p < 3 && zoomlevels[p] <= zoomlevels[3]) {
                    //    e_printf("Corrupt file: non-alpha encoded before alpha, while invisible pixels have undefined RGB values. Not allowed.\n");
                    //    return false;
                    //}
                    //if (nump > 4 && p < 4 && zoomlevels[p] <= zoomlevels[4]) {
                    //    e_printf("Corrupt file: pixels encoded before frame lookback. Not allowed.\n");
                    //    return false;
                    //}
                }
                z = s->zoomlevels[p];
                if (z < 0) {
                    printf("Corrupt file: invalid plane/zoomlevel\n");
                    return AVERROR(EINVAL);
                }
                //if (100 * pixels_done > quality * pixels_todo && endZL == 0) {
                //    v_printf(5,"%lu subpixels done, %lu subpixels todo, quality target %i%% reached (%i%%)\n",(long unsigned)pixels_done,(long unsigned)pixels_todo,(int)quality,(int)(100*pixels_done/pixels_todo));
                //    flif_decode_FLIF2_inner_interpol(images, ranges, p, endZL, -1, scale, zoomlevels, transforms);
                //    return false;
                //}
                if (ff_flif16_ranges_min(s->range, p) < ff_flif16_ranges_max(s->range, p)) {
                    if (the_predictor[p] < 0) {
                        //predictor = metaCoder.read_int(0, MAX_PREDICTOR);
        case 7:
                        MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p, 0, MAX_PREDICTORS, &predictor);
                        ++s->segment;
                    } else {
                        predictor = the_predictor[p];
                    }
                    //if (1<<(z/2) < breakpoints) {
                    //    v_printf(1,"1:%i scale: %li bytes\n",breakpoints,io.ftell());
                    //    breakpoints /= 2;
                    //    options.show_breakpoints = breakpoints;
                    //    if (options.no_full_decode && breakpoints < 2)
                    //        return false;
                    //}
                    //if (1 << (z / 2) < scale) {
                    //    v_printf(5,"%lu subpixels done (out of %lu subpixels at this scale), scale target 1:%i reached\n",(long unsigned)pixels_done,(long unsigned)pixels_todo,scale);
                    //    flif_decode_FLIF2_inner_interpol(images, ranges, p, endZL, -1, scale, zoomlevels, transforms);
                    //    return false;
                    //}
                    //v_printf_tty((endZL==0?2:10),"\r%i%% done [%i/%i] DEC[%i,%ux%u]  ",(int)(100*pixels_done/pixels_todo),i,plane_zoomlevels(images[0], beginZL, endZL)-1,p,images[0].cols(z),images[0].rows(z));

                    //\for (Image& image : images) {
                    //\    image.getPlane(p).prepare_zoomlevel(z);
                    //\}
                    //\if (p>0) for (Image& image : images) {
                    //\        image.getPlane(0).prepare_zoomlevel(z);
                    //\}
                    //\if (p<3 && nump>3) for (Image& image : images) {
                    //\        image.getPlane(3).prepare_zoomlevel(z);
                    //\}
                    for(int fr = 0; fr < s->num_frames; ++fr) {
                        ff_flif16_prepare_zoomlevel(CTX_CAST(s), &s->frames[fr], p, z);
                        if (p > 0)
                            ff_flif16_prepare_zoomlevel(CTX_CAST(s), &s->frames[fr], 0, z);
                        if (p < 3 && s->num_planes > 3)
                            ff_flif16_prepare_zoomlevel(CTX_CAST(s), &s->frames[fr], 3, z);
                    }
                    // ConstantPlane null_alpha(1);
                    // GeneralPlane &alpha = nump > 3 ? images[0].getPlane(3) : null_alpha;
                    s->properties = av_mallocz((s->num_planes > 3 ? properties_rgba_size[s->curr_plane]
                                                                  : properties_rgb_size[s->curr_plane]) 
                                                                  * sizeof(*s->properties));
                    if (!s->properties)
                        return AVERROR(ENOMEM);
                    
        case 8:
                    if (!(z % 2)) {
                        for (uint32_t r = 1; r < ZOOM_HEIGHT(s->height, z); r += 2) {
                            for (int fr = 0; fr < s->num_frames; fr++) {
                                if(ret = flif_decode_plane_zoomlevel_horizontal(s, s->properties, s->range, alpha_plane,
                                   p, z, fr, r, s->alphazero, s->framelookback, predictor, s->ipp))
                                    goto error;
                            // TODO replac lookback
                            }
                        }
                    } else {
                        for (uint32_t r = 1; r < ZOOM_HEIGHT(s->height, z); r++) {
                            for (int fr = 0; fr < s->num_frames; fr++) {
                                if(ret = flif16_decode_plane_zoomlevel_vertical(s, s->properties, s->range, alpha_plane,
                                   p, z, fr, r, s->alphazero, s->framelookback, predictor, s->ipp))
                                    goto error;
                            }
                        }
                    }

                    s->zoomlevels[p]--;
                } else
                    s->zoomlevels[p]--;

                av_freep(&s->properties);
                s->segment = 6;
            } // End For
    } // End Switch

    s->segment = 0;
    return ret;

    need_more_data:
    printf("[%s] Error return", __func__);
    return AVERROR(EAGAIN);

    error:
    printf("[%s] Error return", __func__);
    return ret;
}

#endif

static int flif16_read_pixeldata(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    int ret;
    printf("At:as [%s] %s, %d\n", __func__, __FILE__, __LINE__);
    if((s->ia % 2))
        ret = flif16_read_ni_image(avctx);
    else
        return AVERROR(EINVAL);

    if(!ret)
        s->state = FLIF16_OUTPUT;

    return ret;
}

static int flif16_write_frame(AVCodecContext *avctx, AVFrame *data)
{
    // Refer to libavcodec/bmp.c for an example.
    // ff_set_dimensions(avctx, width, height );
    // avctx->pix_fmt = ...
    // if ((ret = ff_get_buffer(avctx, p, 0)) < 0)
    //     return ret;
    // p->pict_type = AV_PICTURE_TYPE_I;
    // p->key_frame = 1;
    // for(...)
    //     p->data[...] = ..
    // uint32_t temp;
    uint32_t target_frame;
    int ret;
    FLIF16DecoderContext *s = avctx->priv_data;
    ff_set_dimensions(avctx, s->width, s->height);
    s->out_frame->pict_type = AV_PICTURE_TYPE_I;
    //s->out_frame->key_frame = 1;

    printf("<*****> In flif16_write_frame\n");

    /*
    if (s->num_planes  == 1 && s->bpc <= 256) {
        printf("gray8\n");
        avctx->pix_fmt = AV_PIX_FMT_GRAY8;
    } else if (s->num_planes  == 3 && s->bpc <= 256) {
        printf("rgb24\n");
        avctx->pix_fmt = AV_PIX_FMT_RGB24;
    } else if (s->num_planes  == 4 && s->bpc <= 256) {
        printf("rgb32\n");
        avctx->pix_fmt = AV_PIX_FMT_RGB32;
    } else {
        av_log(avctx, AV_LOG_ERROR, "color depth %u and bpc %u not supported\n",
               s->num_planes, s->bpc);
        return AVERROR_PATCHWELCOME;
    }
    */

    if (s->bpc > 65535) {
        av_log(avctx, AV_LOG_ERROR, "depth per channel greater than 16 bits not supported\n");
        return AVERROR_PATCHWELCOME;
    }

    avctx->pix_fmt = flif16_out_frame_type[FFMIN(s->num_planes, 4)][s->bpc > 255];

    if ((ret = ff_reget_buffer(avctx, s->out_frame, 0)) < 0) {
        printf(">>>>>>Couldn't allocate buffer.\n");
        return ret;
    }

    target_frame = (s->frames[s->out_frames_count].seen_before >= 0)
                   ? s->frames[s->out_frames_count].seen_before
                   : s->out_frames_count;

    if (s->num_frames > 1) {
        s->out_frame->pts = s->pts;
        s->pts += s->framedelay[s->out_frames_count];
    }

    printf(">>>>>>>>>target: %d pts: %ld\n", target_frame, s->out_frame->pts);
    printf(">>>>>>>>>Linesize: %d\n", s->out_frame->linesize[0]);

    switch (avctx->pix_fmt) {
        case AV_PIX_FMT_GRAY8:
            for (uint32_t i = 0; i < s->height; ++i) {
                for (uint32_t j = 0; j < s->width; ++j) {
                    *(s->out_frame->data[0] + i * s->out_frame->linesize[0] + j) = \
                    PIXEL_GET(s, target_frame, 0, i, j);
                }
            }
            break;

        case AV_PIX_FMT_RGB24:
            //s->out_frame->linesize[0] = s->width * 3;
            for (uint32_t i = 0; i < s->height; ++i) {
                for (uint32_t j = 0; j < s->width; ++j) {
                    *(s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 3 + 0 ) = \
                    PIXEL_GET(s, target_frame, 0, i, j);
                    //printf("%d ", i * p->linesize[0] * 3 + j * 3);
                    *(s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 3 + 1) = \
                    PIXEL_GET(s, target_frame, 1, i, j);
                    //printf("%d ", i * p->linesize[0] * 3+ j * 3 + 1);
                    *(s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 3 + 2) = \
                    PIXEL_GET(s, target_frame, 2, i, j);
                    //printf("%d \n", i * p->linesize[0] * 3 + j * 3 + 2);
                   /* temp = (0xFF << 24) | ((0xFF & PIXEL_GET(s, s->out_frames_count, 0, i, j)) << 16) |
                    ((0xFF & PIXEL_GET(s, s->out_frames_count, 1, i, j)) << 8) |
                    ((0xFF & PIXEL_GET(s, s->out_frames_count, 2, i, j)));
                    printf("%x (%x %x %x) ", temp,
                     ((0xFF & PIXEL_GET(s, s->out_frames_count, 0, i, j)) << 16),
                    ((0xFF & PIXEL_GET(s, s->out_frames_count, 1, i, j)) << 8),
                    ((0xFF & PIXEL_GET(s, s->out_frames_count, 2, i, j))));
                    *(uint32_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 3) \
                    = temp;*/

                    // = !(j % 3) ? 0x00FF0000 : (j % 3 == 1) ? 0x0000FF00 : 0x000000FF;
                    //for(uint32_t k = 0; k < s->width * s->height * 3; ++k)
                    //    printf("%d ", *(p->data[0] + k));
                    //printf("\n");
                }
            }
            break;

        case AV_PIX_FMT_RGB32:
            for (uint32_t i = 0; i < s->height; ++i) {
                for (uint32_t j = 0; j < s->width; ++j) {
                    *((uint32_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 4))
                    = (PIXEL_GET(s, target_frame, 3, i, j) << 24) |
                      (PIXEL_GET(s, target_frame, 0, i, j) << 16) |
                      (PIXEL_GET(s, target_frame, 1, i, j) << 8)  |
                       PIXEL_GET(s, target_frame, 2, i, j);
                }
            }
            break;

        case AV_PIX_FMT_GRAY16:
            for (uint32_t i = 0; i < s->height; ++i) {
                for (uint32_t j = 0; j < s->width; ++j) {
                    *((uint16_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 2)) = \
                    PIXEL_GET(s, target_frame, 0, i, j);
                }
            }
            break;

        case AV_PIX_FMT_RGB48:
            for (uint32_t i = 0; i < s->height; ++i) {
                for (uint32_t j = 0; j < s->width; ++j) {
                    *((uint16_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 6 + 0)) = \
                    PIXEL_GET(s, target_frame, 0, i, j);
                    *((uint16_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 6 + 1)) = \
                    PIXEL_GET(s, target_frame, 1, i, j);
                    *((uint16_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 6 + 2)) = \
                    PIXEL_GET(s, target_frame, 2, i, j);
                }
            }

        case AV_PIX_FMT_RGBA64:
            for (uint32_t i = 0; i < s->height; ++i) {
                for (uint32_t j = 0; j < s->width; ++j) {
                    *((uint64_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 8))
                    = (uint64_t) \
                      (((uint64_t) PIXEL_GET(s, target_frame, 3, i, j)) << 48) |
                      (((uint64_t) PIXEL_GET(s, target_frame, 2, i, j)) << 32) |
                      (((uint64_t) PIXEL_GET(s, target_frame, 1, i, j)) << 16) |
                       ((uint64_t) PIXEL_GET(s, target_frame, 0, i, j));
                }
            }
            break;

        default:
            av_log(avctx, AV_LOG_ERROR, "Pixel format %d out of bounds?\n", avctx->pix_fmt);
            return AVERROR_PATCHWELCOME;
    }

    av_frame_ref(data, s->out_frame);
    if ((++s->out_frames_count) >= s->num_frames)
        s->state = FLIF16_EOS;
        
    return 0;
}

static int flif16_read_checksum(AVCodecContext *avctx)
{
    return AVERROR_EOF;
}

static int flif16_decode_init(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    s->out_frame = av_frame_alloc();
    if (!s->out_frame)
        return AVERROR(ENOMEM);
    return 0;
}

static int flif16_decode_frame(AVCodecContext *avctx,
                               void *data, int *got_frame,
                               AVPacket *avpkt)
{
    int ret = 0;
    FLIF16DecoderContext *s = avctx->priv_data;
    const uint8_t *buf      = avpkt->data;
    int buf_size            = avpkt->size;
    AVFrame *p              = data;

    bytestream2_init(&s->gb, buf, buf_size);
    printf("[%s] Entering decode\n", __func__);

    // Looping is done to change states in between functions.
    // Function will either exit on AVERROR(EAGAIN) or AVERROR_EOF
    do {
        printf("In: %d\n", s->state);
        switch(s->state) {
            case FLIF16_HEADER:
                ret = flif16_read_header(avctx);
                break;

            case FLIF16_SECONDHEADER:
                ret = flif16_read_second_header(avctx);
                break;

            case FLIF16_TRANSFORM:
                ret = flif16_read_transforms(avctx);
                break;

            case FLIF16_ROUGH_PIXELDATA:
                av_assert0(0);
                ret = flif16_read_pixeldata(avctx);
                if (!ret)
                    s->state = FLIF16_MANIAC;
                break;

            case FLIF16_MANIAC:
                // TODO manage interlaced condition
                printf("{{}{}{}{}{}{Entering MANIAC\n");
                ret = flif16_read_maniac_forest(avctx);
                printf("{{}{}{}{}{}{Exiting MANIAC\n");
                break;

            case FLIF16_PIXELDATA:
                ret = flif16_read_pixeldata(avctx);
                break;

            case FLIF16_CHECKSUM:
                ret = flif16_read_checksum(avctx);
                break;

            case FLIF16_OUTPUT:
                ret = flif16_write_frame(avctx, p);
                if (!ret) {
                    *got_frame = 1;
                    printf("[%s] OUT frames = %d ret = %d\n", __FILE__, s->out_frames_count, buf_size);
                    /*if(s->frames) {
                        for(int k = 0; k < s->num_planes; ++k) {
                            for(int j = 0; j < s->height; ++j) {
                                for(int i = 0; i < s->width; ++i) {
                                    printf("%d ", ff_flif16_pixel_get(CTX_CAST(s), &s->frames[0], k, j, i));
                                }
                                printf("\n");
                            }
                            printf("===\n");
                        }
                    }*/
                    return buf_size;
                }
                break;

            case FLIF16_EOS:
                return AVERROR_EOF;
        }

        printf("[Decode %d] Ret: %d\n", s->state - 1, ret);
    } while (!ret);

    printf("[Decode Result]\n"                  \
           "Width: %u, Height: %u, Frames: %u\n"\
           "ia: %x bpc: %u planes: %u\n"      \
           "alphazero: %u custombc: %u\n"       \
           "cutoff: %u alphadiv: %u \n"         \
           "loops: %u\nl", s->width, s->height, s->num_frames, s->ia, s->bpc,
           s->num_planes, s->alphazero, s->custombc, s->cut,
           s->alpha, s->loops);

    if (s->framedelay) {
        printf("Framedelays:\n");
        for(uint32_t i = 0; i < s->num_frames; ++i)
            printf("%u, ", s->framedelay[i]);
        printf("\n");
    }

   /*if(s->maniac_ctx.forest) {
        printf("Tree Size: %d\n", s->maniac_ctx.forest[0]->size);
        printf("MANIAC Tree first node:\n" \
               "property value: %d\n", s->maniac_ctx.forest[0]->data[0].property);
    }*/

    /*if(s->frames) {
        for(int k = 0; k < s->num_planes; ++k) {
            for(int j = 0; j < s->height; ++j) {
                for(int i = 0; i < s->width; ++i) {
                    printf("%d ", ff_flif16_pixel_get(CTX_CAST(s), &s->frames[0], k, j, i));
                }
                printf("\n");
            }
            printf("===\n");
        }
    }*/
    printf("[%s] buf_size = %d\n", __FILE__, buf_size);
    return ret;
}

static av_cold int flif16_decode_end(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    if (s->framedelay)
        av_freep(&s->framedelay);
    if (s->prop_ranges)
        av_freep(&s->prop_ranges);
    if (s->frames)
        ff_flif16_frames_free(&s->frames, s->num_frames, s->num_planes, s->framelookback);

    //for (int i = s->transform_top - 1; i >= 0; --i)
    //    ff_flif16_transforms_close(s->transforms[i]);

    ff_flif16_maniac_close(&s->maniac_ctx, s->num_planes);
    av_frame_free(&s->out_frame);

    //if (s->range)
    //    ff_flif16_ranges_close(s->range);
    return 0;
}

AVCodec ff_flif16_decoder = {
    .name           = "flif16",
    .long_name      = NULL_IF_CONFIG_SMALL("FLIF (Free Lossless Image Format)"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FLIF16,
    .init           = flif16_decode_init,
    .close          = flif16_decode_end,
    .priv_data_size = sizeof(FLIF16DecoderContext),
    .decode         = flif16_decode_frame,
    .capabilities   = AV_CODEC_CAP_DELAY,
    //.caps_internal  = 0,
    .priv_class     = NULL,
};
