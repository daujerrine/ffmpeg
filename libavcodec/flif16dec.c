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

#include "flif16.h"
#include "flif16_rangecoder.h"
#include "flif16_transform.h"

#include "avcodec.h"
#include "libavutil/common.h"
#include "bytestream.h"
#include "avcodec.h"
#include "internal.h"

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
    
    s->cut   = CHANCETABLE_DEFAULT_CUT;
    s->alpha = CHANCETABLE_DEFAULT_ALPHA;
    
    // Minimum size has been empirically found to be 8 bytes.
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
    s->ia         = temp >> 4;
    s->num_planes = (0x0F & temp);

    if (!(s->ia % 2)) {
        av_log(avctx, AV_LOG_ERROR, "interlaced images not supported\n");
        return AVERROR_PATCHWELCOME;
    }
    
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
    s->width++;
    s->height++;
    (s->ia > 4) ? (s->num_frames += 2) : (s->num_frames = 1);

    if (s->num_frames > 1)
        s->framedelay = av_mallocz(sizeof(*(s->framedelay)) * s->num_frames);

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
            ++s->segment;

        case 6:
            if (s->customalpha) {
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
            if (!flif16_transforms[temp]) {
                av_log(avctx, AV_LOG_ERROR, "transform %u not implemented\n", temp);
                return AVERROR_PATCHWELCOME;
            }

            s->transforms[s->transform_top] = ff_flif16_transform_init(temp, s->range);
            if (!s->transforms[s->transform_top]) {
                av_log(avctx, AV_LOG_ERROR, "failed to initialise transform %u\n", temp);
                return AVERROR(ENOMEM);
            }

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
            if(ff_flif16_transform_read(s->transforms[s->transform_top],
                                        CTX_CAST(s), s->range) <= 0)
                goto need_more_data;
            prev_range = s->range;
            s->range = ff_flif16_transform_meta(CTX_CAST(s), s->frames, s->num_frames,
                                                s->transforms[s->transform_top],
                                                prev_range);
            if(!s->range)
                return AVERROR(ENOMEM);
            s->segment = 0;
            ++s->transform_top;
            goto loop;

        case 3:
            end:
            s->segment = 3;   
            // Read invisible pixel predictor
            if (   s->alphazero && s->num_planes > 3
                && ff_flif16_ranges_min(s->range, 3) <= 0
                && !(s->ia % 2))
                RAC_GET(&s->rc, NULL, 0, 2, &s->ipp, FLIF16_RAC_UNI_INT8)
    }

    for (int i = 0; i < ((s->num_planes > 4) ? 4 : s->num_planes); ++i)
        if (   s->plane_mode[i] != FLIF16_PLANEMODE_NORMAL
            && (ff_flif16_ranges_min(s->range, i) >= ff_flif16_ranges_max(s->range, i))) {
            const_plane_value[i] = ff_flif16_ranges_min(s->range, i);
        }

    if (ff_flif16_planes_init(CTX_CAST(s), s->frames, s->plane_mode,
                          const_plane_value) < 0) {
        av_log(avctx, AV_LOG_ERROR, "could not allocate planes\n");
        return AVERROR(ENOMEM);
    }
        

    s->state = FLIF16_MANIAC;
    s->segment = 0;
    return 0;

    need_more_data:
    return AVERROR(EAGAIN);
}

static int flif16_read_maniac_forest(AVCodecContext *avctx)
{
    int ret;
    FLIF16DecoderContext *s = avctx->priv_data;
    if (!s->maniac_ctx.forest) {
        s->maniac_ctx.forest = av_mallocz((s->num_planes) * sizeof(*(s->maniac_ctx.forest)));
        if (!s->maniac_ctx.forest) {
            return AVERROR(ENOMEM);
        }
        s->segment = s->i = 0; // Remove later
    }
    switch (s->segment) {
        case 0:
            loop:
            if (s->i >= s->num_planes)
                goto end;

            if (!(s->ia % 2))
                s->prop_ranges = ff_flif16_maniac_prop_ranges_init(&s->prop_ranges_size, s->range,
                                                                   s->i, s->num_planes);
            else
                s->prop_ranges = ff_flif16_maniac_ni_prop_ranges_init(&s->prop_ranges_size, s->range,
                                                                      s->i, s->num_planes);

            if(!s->prop_ranges)
                return AVERROR(ENOMEM);
            ++s->segment;

        case 1:
            if (ff_flif16_ranges_min(s->range, s->i) >= ff_flif16_ranges_max(s->range, s->i)) {
                ++s->i;
                --s->segment;
                goto loop;
            }
            ret = ff_flif16_read_maniac_tree(&s->rc, &s->maniac_ctx, s->prop_ranges,
                                             s->prop_ranges_size, s->i);
            if (ret) {
                goto error;
            }
            av_freep(&s->prop_ranges);
            --s->segment;
            ++s->i;
            goto loop;
    }

    end:
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
            properties[index++] = ff_flif16_pixel_get(CTX_CAST(s), pixel, pp, r, c);
        }
        if (ranges_ctx->num_planes > 3) {
            properties[index++] = ff_flif16_pixel_get(CTX_CAST(s), pixel, 3, r, c);
        }
    }
    left = (nobordercases || c > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r, c-1) : 
           (r > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c) : fallback));
    top = (nobordercases || r > 0 ? ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c) : left);
    topleft = (nobordercases || (r>0 && c>0) ? 
              ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c-1) : (r > 0 ? top : left));
    gradientTL = left + top - topleft;
    guess = MEDIAN3(gradientTL, left, top);
    ff_flif16_ranges_snap(ranges_ctx, p, properties, min, max, &guess);

    if (guess == gradientTL)
        which = 0;
    else if (guess == left)
        which = 1;
    else if (guess == top)
        which = 2;

    properties[index++] = guess;
    properties[index++] = which;

    if (nobordercases || (c > 0 && r > 0)) {
        properties[index++] = left - topleft;
        properties[index++] = topleft - top;
    } else {
        properties[index++] = 0;
        properties[index++] = 0; 
    }

    if (nobordercases || (c+1 < width && r > 0)) {
        properties[index++] = top - ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-1, c+1); // top - topright 
    } else {
        properties[index++] = 0;
    }

    if (nobordercases || r > 1) {
        properties[index++] = ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r-2, c) - top;  // toptop - top
    } else {
        properties[index++] = 0;
    }

    if (nobordercases || c > 1) {
        properties[index++] = ff_flif16_pixel_get(CTX_CAST(s), pixel, p, r, c-2) - left;  // leftleft - left
    } else {
        properties[index++] = 0;
    }

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
    return MEDIAN3(gradientTL, left, top);
}

static int flif16_read_ni_plane(FLIF16DecoderContext *s,
                                FLIF16RangesContext *ranges_ctx,
                                FLIF16ColorVal *properties, uint8_t p,
                                uint32_t fr, uint32_t r, FLIF16ColorVal gray,
                                FLIF16ColorVal minP)
{
    FLIF16ColorVal curr;
    uint32_t begin = 0, end = s->width;
    switch (s->segment2) {
        case 0:
            // if this is a duplicate frame, copy the row from the frame being duplicated
            // TODO put condition in flif16 read ni image instead
            if (s->frames[fr].seen_before >= 0) {
                return 0;
            }

            // if this is not the first or only frame, fill the beginning of the row
            // before the actual pixel data
            if (fr > 0) {
                //if alphazero is on, fill with a predicted value, otherwise
                // copy pixels from the previous frame
                begin = (!s->frameshape) ? 0 : s->frames[fr].col_begin[r];
                end = (!s->frameshape) ? s->width : s->frames[fr].col_end[r];
                if (s->alphazero && p < 3) {
                    for (uint32_t c = 0; c < begin; c++)
                        if (PIXEL_GET(s, fr, 3, r, c) == 0)
                            PIXEL_SET(s, fr, p, r, c, flif16_ni_predict(s, &s->frames[fr], p, r, c, gray));
                        else
                            PIXEL_SET(s, fr, p, r, c, PIXEL_GET(s, PREV_FRAMENUM(s->frames, fr), p, r, c)); 
                } else if (p != 4) {
                    ff_flif16_copy_rows(CTX_CAST(s), &s->frames[fr],
                                        PREV_FRAME(s->frames, fr), p, r, 0, begin);
                }
            }
            ++s->segment2;

            if (r > 1 && !s->framelookback && begin == 0 && end > 3) {
            s->c = begin;
            
            for (; s->c < 2; s->c++) {
                if (s->alphazero && p<3 &&
                    PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                    //printf("<aa> 1\n");
                    PIXEL_SET(s, fr, p, r, s->c,
                    flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                    continue;
                }
                s->guess = flif16_ni_predict_calcprops(s, &s->frames[fr],
                           properties, ranges_ctx, p, r, s->c, &s->min, &s->max, minP, 0);
        case 1:
                MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                ff_flif16_pixel_set(CTX_CAST(s), &s->frames[fr], p, r, s->c, curr);
            }
            ++s->segment2;

            for (; s->c < end-1; s->c++) {
                if (s->alphazero && p < 3 &&
                    ff_flif16_pixel_get(CTX_CAST(s), &s->frames[fr], 3, r, s->c) == 0) {
                    ff_flif16_pixel_set(CTX_CAST(s),&s->frames[fr], p, r, s->c,
                    flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                    continue;
                }
                s->guess = flif16_ni_predict_calcprops(s, &s->frames[fr],
                           properties, ranges_ctx, p, r, s->c, &s->min, &s->max, minP, 1);
        case 2:
                MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SET(s, fr, p, r, s->c, curr);
            }
            ++s->segment2;

            for (; s->c < end; s->c++) {
                if (s->alphazero && p < 3 &&
                    PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                    PIXEL_SET(s, fr, p, r, s->c, flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                    continue;
                }
               s->guess = flif16_ni_predict_calcprops(s, &s->frames[fr],
                          properties, ranges_ctx, p, r, s->c, &s->min, &s->max, minP, 0);
        case 3:
                MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SET(s, fr, p, r, s->c, curr);
            }
            ++s->segment2;

            } else {
                s->segment2 = 4;
                for (s->c = begin; s->c < end; s->c++) {
                    if (s->alphazero && p < 3 &&
                        ff_flif16_pixel_get(CTX_CAST(s), &s->frames[fr], 3, r, s->c) == 0) {
                        PIXEL_SET(s, fr, p, r, s->c,
                        flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                        continue;
                    }
                    if (s->framelookback && p < 4 &&
                        PIXEL_GET(s, fr, FLIF16_PLANE_LOOKBACK, r, s->c) > 0) {
                        // TODO accomodate PRE_FRAME for this
                        PIXEL_SET(s, fr, p, r, s->c,
                        PIXEL_GET(s, fr - PIXEL_GET(s, fr, FLIF16_PLANE_LOOKBACK, r, s->c), p, r, s->c));
                        continue;
                    }
                    //calculate properties and use them to decode the next pixel
                    s->guess = flif16_ni_predict_calcprops(s, &s->frames[fr], properties,
                                                           ranges_ctx, p, r, s->c, &s->min,
                                                           &s->max, minP, 0);
                    if (s->framelookback && p == FLIF16_PLANE_LOOKBACK && s->max > fr)
                        s->max = fr;
        case 4:
                    MANIAC_GET(&s->rc, &s->maniac_ctx, properties, p,
                               s->min - s->guess, s->max - s->guess, &curr);
                    curr += s->guess;
                    PIXEL_SET(s, fr, p, r, s->c, curr);
                }
            } // End If

            // If this is not the first or only frame, fill the end of the row after the actual pixel data
            if (fr > 0) {
                //if alphazero is on, fill with a predicted value, otherwise copy pixels from the previous frame
                if (s->alphazero && p < 3) {
                    for (uint32_t c = end; c < s->width; c++)
                        if (PIXEL_GET(s, fr, 3, r, s->c) == 0)
                            PIXEL_SET(s, fr,  p, r, s->c,
                            flif16_ni_predict(s, &s->frames[fr], p, r, s->c, gray));
                        else
                            PIXEL_SET(s, fr, p, r, s->c, PIXEL_GET(s, PREV_FRAMENUM(s->frames, fr), p, r, s->c));
                } else if(p != 4) {
                     ff_flif16_copy_rows(CTX_CAST(s), &s->frames[fr],
                     PREV_FRAME(s->frames, fr), p, r, end, s->width);
                }
            }
    }

    s->segment2 = 0;
    return 0;

    need_more_data:
    return AVERROR(EAGAIN);
}


static FLIF16ColorVal *compute_grays(FLIF16RangesContext *ranges)
{
    FLIF16ColorVal *grays; // a pixel with values in the middle of the bounds
    grays = av_malloc(ranges->num_planes * sizeof(*grays));
    for (int p = 0; p < ranges->num_planes; p++)
        grays[p] = (ff_flif16_ranges_min(ranges, p) + ff_flif16_ranges_max(ranges, p)) / 2;
    return grays;
}

static int flif16_read_ni_image(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    int ret;
    FLIF16ColorVal min_p;

    switch (s->segment) {
        case 0:
            s->grays = compute_grays(s->range); // free later
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
                if (ff_flif16_ranges_min(s->range, s->curr_plane) >=
                    ff_flif16_ranges_max(s->range, s->curr_plane)) {
                    continue;
                }
                s->properties = av_mallocz((s->num_planes > 3 ? properties_ni_rgba_size[s->curr_plane]
                                                            : properties_ni_rgb_size[s->curr_plane]) 
                                                            * sizeof(*s->properties));
                for (; s->i2 < s->height; ++s->i2) {
                    for (; s->i3 < s->num_frames; ++s->i3) {
        case 1:
                        min_p = ff_flif16_ranges_min(s->range, s->curr_plane);
                        ret = flif16_read_ni_plane(s, s->range, s->properties,
                                                   s->curr_plane,
                                                   s->i3,
                                                   s->i2,
                                                   s->grays[s->curr_plane],
                                                   min_p);
                        
                        if (ret) {
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

    s->state = FLIF16_OUTPUT;
    return 0;

    error:
    return ret;
}

static int flif16_read_pixeldata(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    int ret;
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
    uint32_t target_frame;
    int ret;
    FLIF16DecoderContext *s = avctx->priv_data;
    ff_set_dimensions(avctx, s->width, s->height);
    s->out_frame->pict_type = AV_PICTURE_TYPE_I;

    if (s->bpc > 65535) {
        av_log(avctx, AV_LOG_ERROR, "depth per channel greater than 16 bits not supported\n");
        return AVERROR_PATCHWELCOME;
    }

    avctx->pix_fmt = flif16_out_frame_type[FFMIN(s->num_planes, 4)][s->bpc > 255];

    if ((ret = ff_reget_buffer(avctx, s->out_frame, 0)) < 0) {
        return ret;
    }

    target_frame = (s->frames[s->out_frames_count].seen_before >= 0)
                   ? s->frames[s->out_frames_count].seen_before
                   : s->out_frames_count;

    if (s->num_frames > 1) {
        s->out_frame->pts = s->pts;
        s->pts += s->framedelay[s->out_frames_count];
    }

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
            for (uint32_t i = 0; i < s->height; ++i) {
                for (uint32_t j = 0; j < s->width; ++j) {
                    *(s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 3 + 0 ) = \
                    PIXEL_GET(s, target_frame, 0, i, j);
                    *(s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 3 + 1) = \
                    PIXEL_GET(s, target_frame, 1, i, j);
                    *(s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 3 + 2) = \
                    PIXEL_GET(s, target_frame, 2, i, j);
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

    // Looping is done to change states in between functions.
    // Function will either exit on AVERROR(EAGAIN) or AVERROR_EOF
    do {
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
                ret = flif16_read_maniac_forest(avctx);
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
                    return buf_size;
                }
                break;

            case FLIF16_EOS:
                return AVERROR_EOF;
        }

    } while (!ret);

    return ret;
}

static av_cold int flif16_decode_end(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    if(s->framedelay)
        av_freep(&s->framedelay);
    if (s->prop_ranges)
        av_freep(&s->prop_ranges);
    if (s->frames)
        ff_flif16_frames_free(&s->frames, s->num_frames, s->num_planes);

    for (int i = s->transform_top - 1; i >= 0; --i)
        ff_flif16_transforms_close(s->transforms[i]);

    ff_flif16_maniac_close(&s->maniac_ctx, s->num_planes);
    av_frame_free(&s->out_frame);

    if (s->range)
        ff_flif16_ranges_close(s->range);
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
