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
#include "bytestream.h"
#include "internal.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"

typedef enum FLIF16DecodeStates {
    FLIF16_HEADER = 0,
    FLIF16_SECONDHEADER,
    FLIF16_TRANSFORM,
    FLIF16_ROUGH_PIXELDATA,
    FLIF16_MANIAC,
    FLIF16_PIXELDATA,
    FLIF16_OUTPUT,
    FLIF16_EOS
} FLIF16DecodeStates;

/*
 * Due to the nature of the format, the decoder has to take the entirety of the
 * data before it can generate any frames. The decoder has to return
 * AVERROR(EAGAIN) as long as the bitstream is incomplete.
 */

typedef struct FLIF16DecoderContext {

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

    /* End Inheritance from FLIF16Context */

    AVFrame *out_frame;
    FLIF16PixelData  *frames;
    GetByteContext gb;
    int64_t pts;
    uint32_t out_frames_count;

    FLIF16DecodeStates state; ///< The section of the file the parser is in currently.
    unsigned int segment;     ///< The "segment" the code is supposed to jump to
    unsigned int segment2;
    int i;                    ///< A generic iterator used to save states between for loops.
    int i2;
    int i3;

    // Secondary Header
    uint8_t alphazero;   ///< Alphazero Flag
    uint8_t custombc;    ///< Custom Bitchance Flag
    uint32_t alpha;      ///< Chancetable custom alphadivisor
    uint8_t customalpha; ///< Custom alphadiv & cutoff flag
    uint8_t cut;         ///< Chancetable custom cutoff
    uint8_t ipp;         ///< Invisible pixel predictor

    // Transforms
    uint8_t transform_top;
    FLIF16TransformContext *transforms[MAX_TRANSFORMS];
    FLIF16RangesContext *range; ///< The minimum and maximum values a
                                ///  channel's pixels can take. Changes
                                ///  depending on transformations applied

    // MANIAC Trees
    FLIF16MinMax prop_ranges[MAX_PROP_RANGES]; ///< Property Ranges
    uint32_t prop_ranges_size;

    // Pixeldata
    FLIF16ColorVal grays[MAX_PLANES];
    FLIF16ColorVal properties[MAX_PROPERTIES];
    FLIF16ColorVal guess;    ///< State variable. Stores guess
    FLIF16ColorVal min, max;
    uint32_t begin;          ///< State variable for Column range end
    uint32_t end;            ///< State variable for Column range start
    uint32_t c;              ///< State variable for current column
    uint8_t curr_plane;      ///< State variable. Current plane under processing

    // Interlaced Pixeldata
    uint8_t default_order;
    int begin_zl;
    int rough_zl;
    int end_zl;
    int curr_zoom;
    int zoomlevels[MAX_PLANES];
    int predictors[MAX_PLANES];
    int predictor;
} FLIF16DecoderContext;

// Cast values to FLIF16Context for some functions.
#define CTX_CAST(x) ((FLIF16Context *) (x))

#define PIXEL_SET(ctx, fr, p, r, c, val) ff_flif16_pixel_set(CTX_CAST(ctx), &(ctx)->frames[fr], p, r, c, val)
#define PIXEL_GET(ctx, fr, p, r, c) ff_flif16_pixel_get(CTX_CAST(ctx), &(ctx)->frames[fr], p, r, c)
#define PIXEL_SETZ(ctx, fr, p, z, r, c, val) ff_flif16_pixel_setz(CTX_CAST(ctx), &(ctx)->frames[fr], p, z, r, c, val)
#define PIXEL_GETZ(ctx, fr, p, z, r, c) ff_flif16_pixel_getz(CTX_CAST(ctx), &(ctx)->frames[fr], p, z, r, c)
#define PIXEL_GETFAST(ctx, fr, p, r, c) ff_flif16_pixel_get_fast(CTX_CAST(ctx), &(ctx)->frames[fr], p, r, c)
#define PIXEL_SETFAST(ctx, fr, p, r, c, val) ff_flif16_pixel_set_fast(CTX_CAST(ctx), &(ctx)->frames[fr], p, r, c, val)

#define PREV_FRAME(frames, f_no) (((frames)[(f_no) - 1].seen_before >= 0) ? &(frames)[(frames)[(f_no) - 1].seen_before] : &(frames)[(f_no) - 1])
#define PREV_FRAMENUM(frames, f_no) (((frames)[(f_no) - 1].seen_before >= 0) ? (frames)[(f_no) - 1].seen_before : (f_no) - 1)
#define LOOKBACK_FRAMENUM(ctx, frames, f_no, r, c) (((frames)[(f_no) - PIXEL_GET((ctx), (f_no), FLIF16_PLANE_LOOKBACK, (r), (c))].seen_before >= 0) ? \
                                                    ((frames)[(f_no) - PIXEL_GET((ctx), (f_no), FLIF16_PLANE_LOOKBACK, (r), (c))].seen_before) : \
                                                    ((f_no) - PIXEL_GET((ctx), (f_no), FLIF16_PLANE_LOOKBACK, (r), (c))))
#define LOOKBACK_FRAMENUMZ(ctx, frames, f_no, z, r, c) (((frames)[(f_no) - PIXEL_GETZ((ctx), (f_no), FLIF16_PLANE_LOOKBACK, (z), (r), (c))].seen_before >= 0) ? \
                                                       ((frames)[(f_no) - PIXEL_GETZ((ctx), (f_no), FLIF16_PLANE_LOOKBACK, (z), (r), (c))].seen_before) : \
                                                       ((f_no) - PIXEL_GETZ((ctx), (f_no), FLIF16_PLANE_LOOKBACK, (z), (r), (c))))

#define IS_CONSTANT(ranges, plane) (ff_flif16_ranges_min((ranges), (plane)) >= \
                                    ff_flif16_ranges_max((ranges), (plane)))

/*
 * From reference decoder:
 *
 * The order in which the planes are encoded.
 * 0: lookback (Lookback) (animations-only, value refers to a previous frame) has
 *    to be first, because all other planes are not encoded if lookback != 0
 * 1: Alpha has to be next, because for fully transparent A=0 pixels, the other
 *    planes are not encoded
 * 2: Y (luma) is next (the first channel for still opaque images), because it is
 *    perceptually most important
 * 3, 4: Co and Cg are in that order because Co is perceptually slightly more
 *       important than Cg [citation needed]
 */
static const int plane_ordering[] = {
    FLIF16_PLANE_LOOKBACK,
    FLIF16_PLANE_ALPHA,
    FLIF16_PLANE_Y,
    FLIF16_PLANE_CO,
    FLIF16_PLANE_CG
};

static int flif16_read_header(AVCodecContext *avctx)
{
    int ret;
    uint8_t temp, count = 4;
    uint8_t header[4];
    FLIF16DecoderContext *s = avctx->priv_data;
    uint32_t *vlist[] = { &s->width, &s->height, &s->num_frames };

    s->cut   = CHANCETABLE_DEFAULT_CUT;
    s->alpha = CHANCETABLE_DEFAULT_ALPHA;

    // Minimum size has been empirically found to be 8 bytes.
    if (bytestream2_size(&s->gb) < 8) {
        av_log(avctx, AV_LOG_ERROR, "buf size too small (%d)\n",
               bytestream2_size(&s->gb));
        return AVERROR_INVALIDDATA;
    }

    bytestream2_get_bufferu(&s->gb, header, 4);

    if (memcmp(header, flif16_header, 4)) {
        av_log(avctx, AV_LOG_ERROR, "bad magic number\n");
        return AVERROR_INVALIDDATA;
    }

    s->state = FLIF16_HEADER;

    temp = bytestream2_get_byte(&s->gb);
    s->ia         = temp >> 4;
    s->num_planes = (0x0F & temp);

    s->bpc = bytestream2_get_byte(&s->gb);



    // Handle dimensions and frames
    for(int i = 0; i < 2 + ((s->ia > 4) ? 1 : 0); i++) {
        while ((temp = bytestream2_get_byte(&s->gb)) > 127) {
            VARINT_APPEND(*vlist[i], temp);
            if (!(count--)) {
                return AVERROR(ENOMEM);
            }
        }
        VARINT_APPEND(*vlist[i], temp);
        count = 4;
    }

    s->width++;
    s->height++;
    (s->ia > 4) ? (s->num_frames += 2) : (s->num_frames = 1);

    // Check for multiplication overflow
    if ((ret = av_image_check_size2(s->width, s->height, avctx->max_pixels,
        AV_PIX_FMT_NONE, 0, avctx)) < 0)
        return ret;

    if (s->num_frames > 1) {
        s->framedelay = av_malloc_array(s->num_frames, sizeof(*(s->framedelay)));
        if (!s->framedelay)
            return AVERROR(ENOMEM);
    }

    s->frames = ff_flif16_frames_init(s->num_frames);

    if (!s->frames)
        return AVERROR(ENOMEM);

    // Handle Metadata Chunk
    while ((temp = bytestream2_get_byte(&s->gb)) != 0) {
        bytestream2_seek(&s->gb, 3, SEEK_CUR);
        while ((temp = bytestream2_get_byte(&s->gb)) > 127) {
            VARINT_APPEND(s->meta, temp);
            if (!(count--)) {
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
    int ret;
    uint32_t temp;
    FLIF16DecoderContext *s = avctx->priv_data;

    switch (s->segment) {
    case 0:
        if ((ret = ff_flif16_rac_init(&s->rc, &s->gb)) < 0)
            return ret;
        s->segment++;

    case 1:
        /*
         * In original source this is handled in what seems to be a very
         * bogus manner. It takes all the bpps of all planes and then
         * takes the max, negating any benefit of actually keeping these
         * multiple values.
         */
        if (s->bpc == '0') {
            s->bpc = 0;
            for (; s->i < s->num_planes; s->i++) {
                RAC_GET(&s->rc, NULL, 1, 15, &temp, FLIF16_RAC_UNI_INT8);
                s->bpc = FFMAX(s->bpc, (1 << temp) - 1);
            }
        } else
            s->bpc = (s->bpc == '1') ? 255 : 65535;
        s->i = 0;
        s->range = ff_flif16_ranges_static_init(s->num_planes, s->bpc);
        s->segment++;

    case 2:
        if (s->num_planes > 3) {
            RAC_GET(&s->rc, NULL, 0, 1, &s->alphazero, FLIF16_RAC_UNI_INT8);
        }
        s->segment++;

    case 3:
        if (s->num_frames > 1) {
            RAC_GET(&s->rc, NULL, 0, 100, &s->loops, FLIF16_RAC_UNI_INT8);
        }
        s->segment++;

    case 4:
        if (s->num_frames > 1) {
            for (; (s->i) < (s->num_frames); s->i++) {
                RAC_GET(&s->rc, NULL, 0, 60000, &(s->framedelay[s->i]),
                        FLIF16_RAC_UNI_INT16);
            }
            s->i = 0;
        }
        s->segment++;

    case 5:
        // Has custom alpha flag
        RAC_GET(&s->rc, NULL, 0, 1, &s->customalpha, FLIF16_RAC_UNI_INT8);
        s->segment++;

    case 6:
        if (s->customalpha) {
            RAC_GET(&s->rc, NULL, 1, 128, &s->cut, FLIF16_RAC_UNI_INT8);
        }
        s->segment++;

    case 7:
        if (s->customalpha) {
            RAC_GET(&s->rc, NULL, 2, 128, &s->alpha, FLIF16_RAC_UNI_INT8);
            s->alpha = 0xFFFFFFFF / s->alpha;
        }
        s->segment++;

    case 8:
        if (s->customalpha)
            RAC_GET(&s->rc, NULL, 0, 1, &s->custombc, FLIF16_RAC_UNI_INT8);
        if (s->custombc) {
            av_log(avctx, AV_LOG_ERROR, "custom bitchances not implemented\n");
            return AVERROR_PATCHWELCOME;
        }
    }

    s->state   = FLIF16_TRANSFORM;
    s->segment = 0;

#ifdef MULTISCALE_CHANCES_ENABLED
    s->rc.mct = ff_flif16_multiscale_chancetable_init();
    ff_flif16_build_log4k_table(&s->rc.log4k);
#endif

    ff_flif16_chancetable_init(&s->rc.ct, s->alpha, s->cut);

    return 0;

need_more_data:
    return AVERROR(EAGAIN);
}


static int flif16_read_transforms(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    FLIF16RangesContext *prev_range;
    int ret;
    int unique_frames;
    int32_t const_plane_value[MAX_PLANES];
    uint8_t temp;

    switch (s->segment) {
        while (1) {
    case 0:
            RAC_GET(&s->rc, NULL, 0, 0, &temp, FLIF16_RAC_BIT);
            if(!temp)
                break;
            s->segment++;

    case 1:
            RAC_GET(&s->rc, NULL, 0, MAX_TRANSFORMS, &temp, FLIF16_RAC_UNI_INT8);
            if (!flif16_transforms[temp]) {
                av_log(avctx, AV_LOG_ERROR, "transform %u not implemented\n", temp);
                return AVERROR_PATCHWELCOME;
            }

            s->transforms[s->transform_top] = ff_flif16_transform_init(temp, s->range);
            if (!s->transforms[s->transform_top])
                return AVERROR(ENOMEM);

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
                s->plane_mode[FLIF16_PLANE_Y]  = FLIF16_PLANEMODE_NORMAL;
                s->plane_mode[FLIF16_PLANE_CO] = FLIF16_PLANEMODE_NORMAL;
                s->plane_mode[FLIF16_PLANE_CG] = FLIF16_PLANEMODE_NORMAL;
                break;

            case FLIF16_TRANSFORM_DUPLICATEFRAME:
                s->framedup = 1;
                if(s->num_frames < 2)
                     return AVERROR_INVALIDDATA;
                ff_flif16_transform_configure(s->transforms[s->transform_top],
                                              s->num_frames);
                break;

            case FLIF16_TRANSFORM_FRAMESHAPE:
                s->frameshape = 1;
                if (s->num_frames < 2)
                    return AVERROR_INVALIDDATA;
                unique_frames = s->num_frames - 1;
                for (unsigned int i = 0; i < s->num_frames; i++) {
                    if(s->frames[i].seen_before >= 0)
                        unique_frames--;
                }
                if (unique_frames < 1)
                    return AVERROR_INVALIDDATA;
                ff_flif16_transform_configure(s->transforms[s->transform_top],
                                              (unique_frames) * s->height);
                ff_flif16_transform_configure(s->transforms[s->transform_top],
                                              s->width);
                break;

            case FLIF16_TRANSFORM_FRAMELOOKBACK:
                if(s->num_frames < 2)
                    return AVERROR_INVALIDDATA;
                s->framelookback = 1;

                ff_flif16_transform_configure(s->transforms[s->transform_top],
                                              s->num_frames);
                break;
            }
            s->segment++;

    case 2:
            if(ff_flif16_transform_read(CTX_CAST(s), s->transforms[s->transform_top],
                                        s->range) <= 0)
                goto need_more_data;
            prev_range = s->range;
            s->range = ff_flif16_transform_meta(CTX_CAST(s), s->frames, s->num_frames,
                                                s->transforms[s->transform_top],
                                                prev_range);
            if(!s->range)
                return AVERROR(ENOMEM);
            s->segment = 0;
            s->transform_top++;
        }

    case 3:
        s->segment = 3;
        // Read invisible pixel predictor
        if (s->alphazero && s->num_planes > 3 &&
            ff_flif16_ranges_min(s->range, 3) <= 0 &&
            !(s->ia % 2))
            RAC_GET(&s->rc, NULL, 0, 2, &s->ipp, FLIF16_RAC_UNI_INT8)
    }

    for (int i = 0; i < FFMIN(s->num_planes, 4); i++) {
        if (s->plane_mode[i] != FLIF16_PLANEMODE_NORMAL) {
            if (ff_flif16_ranges_min(s->range, i) >= ff_flif16_ranges_max(s->range, i))
                const_plane_value[i] = ff_flif16_ranges_min(s->range, i);
            else
                s->plane_mode[i] = FLIF16_PLANEMODE_NORMAL;
        }
    }

    s->plane_mode[FLIF16_PLANE_LOOKBACK] = FLIF16_PLANEMODE_FILL;
    const_plane_value[FLIF16_PLANE_LOOKBACK] = 0;
    for (int i = 0; i < s->num_frames; i++) 
        if ((ret = ff_flif16_planes_init(CTX_CAST(s), &s->frames[i],
            const_plane_value)) < 0) {
            return ret;
        }

    if (!(s->ia % 2))
        s->state = FLIF16_ROUGH_PIXELDATA;
    else
        s->state = FLIF16_MANIAC;
    s->segment = 0;
    return 0;

need_more_data:
    return AVERROR(EAGAIN);
}

/**
 * Used for decoding rough pixeldata
 */
static int flif16_blank_maniac_forest_init(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    s->maniac_ctx.forest = av_mallocz((s->num_planes) * sizeof(*(s->maniac_ctx.forest)));
    if (!s->maniac_ctx.forest)
        return AVERROR(ENOMEM);

    for (int i = 0; i < s->num_planes; i++) {
        s->maniac_ctx.forest[i] = av_mallocz(sizeof(*(s->maniac_ctx.forest[i])));
        if (!s->maniac_ctx.forest[i])
            return AVERROR(ENOMEM);
        s->maniac_ctx.forest[i]->data = av_mallocz(sizeof(*(s->maniac_ctx.forest[i]->data)));
        if (!s->maniac_ctx.forest[i]->data)
            return AVERROR(ENOMEM);
        s->maniac_ctx.forest[i]->data[0].property = -1;
    }

    return 0;
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
        s->segment = s->i = 0;
    }

    switch (s->segment) {
        for (;s->i < s->num_planes; s->i++) {
    case 0:
            if (!(s->ia % 2))
                ff_flif16_maniac_prop_ranges_init(s->prop_ranges, &s->prop_ranges_size, s->range,
                                                  s->i, s->num_planes);
            else
                ff_flif16_maniac_ni_prop_ranges_init(s->prop_ranges, &s->prop_ranges_size, s->range,
                                                     s->i, s->num_planes);
            s->segment++;

    case 1:
            if (IS_CONSTANT(s->range, s->i)) {
                s->segment--;
                continue;
            }
            ret = ff_flif16_read_maniac_tree(&s->rc, &s->maniac_ctx, s->prop_ranges,
                                             s->prop_ranges_size, s->i);
            if (ret)
                goto error;
            s->segment--;
        }
    }

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


static inline FLIF16ColorVal flif16_ni_predict_calcprops(FLIF16DecoderContext *s,
                                                         uint32_t fr, uint8_t p,
                                                         uint32_t r,
                                                         FLIF16ColorVal fallback,
                                                         uint8_t nobordercases)
{
    FLIF16ColorVal guess, left, top, topleft, gradientTL;
    int which = 0;
    int index = 0;

    if (p < 3) {
        for (int pp = 0; pp < p; pp++) {
            s->properties[index++] = PIXEL_GET(s, fr, pp, r, s->c);
        }
        if (s->range->num_planes > 3) {
            s->properties[index++] = PIXEL_GET(s, fr, 3, r, s->c);
        }
    }

    left = (nobordercases || s->c > 0 ? PIXEL_GET(s, fr, p, r, s->c - 1) :
           (r > 0 ? PIXEL_GET(s, fr, p, r - 1, s->c) : fallback));
    top = (nobordercases || r > 0 ? PIXEL_GET(s, fr, p, r - 1, s->c) : left);
    topleft = (nobordercases || (r > 0 && s->c > 0) ? PIXEL_GET(s, fr, p, r - 1, s->c - 1) : (r > 0 ? top : left));
    gradientTL = left + top - topleft;
    guess = MEDIAN3(gradientTL, left, top);
    ff_flif16_ranges_snap(s->range, p, s->properties, &s->min, &s->max, &guess);

    if (guess == gradientTL)
        which = 0;
    else if (guess == left)
        which = 1;
    else if (guess == top)
        which = 2;

    s->properties[index++] = guess;
    s->properties[index++] = which;

    if (nobordercases || (s->c > 0 && r > 0)) {
        s->properties[index++] = left - topleft;
        s->properties[index++] = topleft - top;
    } else {
        s->properties[index++] = 0;
        s->properties[index++] = 0;
    }

    if (nobordercases || (s->c + 1 < s->width && r > 0)) {
        s->properties[index++] = top - PIXEL_GET(s, fr, p, r - 1, s->c + 1);
    } else {
        s->properties[index++] = 0;
    }

    if (nobordercases || r > 1) {
        s->properties[index++] = PIXEL_GET(s, fr, p, r - 2, s->c) - top;
    } else {
        s->properties[index++] = 0;
    }

    if (nobordercases || s->c > 1) {
        s->properties[index++] = PIXEL_GET(s, fr, p, r, s->c - 2) - left;
    } else {
        s->properties[index++] = 0;
    }

    return guess;
}

static inline FLIF16ColorVal flif16_ni_predict(FLIF16DecoderContext *s,
                                               uint32_t fr, uint32_t p,
                                               uint32_t r)
{
    uint32_t gray = s->grays[p];
    FLIF16ColorVal left = (s->c > 0 ? PIXEL_GET(s, fr, p, r, s->c - 1) :
                          (r > 0 ? PIXEL_GET(s, fr, p, r - 1, s->c) : gray));
    FLIF16ColorVal top = (r > 0 ? PIXEL_GET(s, fr, p, r - 1, s->c) : left);
    FLIF16ColorVal topleft = (r > 0 && s->c > 0 ? PIXEL_GET(s, fr, p, r - 1, s->c - 1) : top);
    FLIF16ColorVal gradientTL = left + top - topleft;
    return MEDIAN3(gradientTL, left, top);
}

static int flif16_read_ni_plane_row(FLIF16DecoderContext *s, uint8_t p, uint32_t fr,
                                    uint32_t r)
{
    FLIF16ColorVal curr;
    FLIF16ColorVal min_p = ff_flif16_ranges_min(s->range, p);

    switch (s->segment2) {
    case 0:
        if (s->frames[fr].seen_before >= 0) {
            return 0;
        }

        // If this is not the first or only frame, fill the beginning of the row
        // before the actual pixel data
        if (fr > 0) {
            // If alphazero is on, fill with a predicted value, otherwise
            // copy pixels from the previous frame

            s->begin = (!s->frameshape) ? 0 : s->frames[fr].col_begin[r];
            s->end   = (!s->frameshape) ? s->width : s->frames[fr].col_end[r];
            if (s->alphazero && p < 3) {
                for (uint32_t c = 0; c < s->begin; c++)
                    if (PIXEL_GET(s, fr, 3, r, c) == 0) {
                        PIXEL_SET(s, fr, p, r, c, flif16_ni_predict(s, fr, p, r));
                    } else {
                        PIXEL_SET(s, fr, p, r, c, PIXEL_GET(s, PREV_FRAMENUM(s->frames, fr), p, r, c));
                    }
            } else if (p != 4) {
                ff_flif16_copy_cols(CTX_CAST(s), &s->frames[fr],
                                    PREV_FRAME(s->frames, fr), p, r, 0, s->begin);
            }
        } else {
            s->begin = 0;
            s->end   = s->width;
        }
        s->segment2++;

        if (r > 1 && !s->framelookback && s->begin == 0 && s->end > 3) {
            // Decode actual pixel data
            s->c = s->begin;

            for (; s->c < 2; s->c++) {
                if (s->alphazero && p < 3 &&
                    PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                    PIXEL_SET(s, fr, p, r, s->c, flif16_ni_predict(s, fr, p, r));
                    continue;
                }
                s->guess = flif16_ni_predict_calcprops(s, fr, p, r, min_p, 0);
    case 1:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                ff_flif16_pixel_set(CTX_CAST(s), &s->frames[fr], p, r, s->c, curr);
            }
            s->segment2++;

            for (; s->c < s->end - 1; s->c++) {
                if (s->alphazero && p < 3 &&
                    PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                    PIXEL_SET(s, fr, p, r, s->c, flif16_ni_predict(s, fr, p, r));
                    continue;
                }
                s->guess = flif16_ni_predict_calcprops(s, fr, p, r, min_p, 1);
    case 2:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SET(s, fr, p, r, s->c, curr);
            }
            s->segment2++;

            for (; s->c < s->end; s->c++) {
                if (s->alphazero && p < 3 &&
                    PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                    PIXEL_SET(s, fr, p, r, s->c, flif16_ni_predict(s, fr, p, r));
                    continue;
                }
                s->guess = flif16_ni_predict_calcprops(s, fr, p, r, min_p, 0);
    case 3:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SET(s, fr, p, r, s->c, curr);
            }
            s->segment2++;

        } else {
            s->segment2 = 4;
            for (s->c = s->begin; s->c < s->end; s->c++) {
                if (s->alphazero && p < 3 &&
                    PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                    PIXEL_SET(s, fr, p, r, s->c, flif16_ni_predict(s, fr, p, r));
                    continue;
                }
                if (s->framelookback && p < 4 &&
                    PIXEL_GET(s, fr, FLIF16_PLANE_LOOKBACK, r, s->c) > 0) {
                    PIXEL_SET(s, fr, p, r, s->c,
                    PIXEL_GET(s, LOOKBACK_FRAMENUM(s, s->frames, fr, r, s->c), p, r, s->c));
                    continue;
                }
                s->guess = flif16_ni_predict_calcprops(s, fr, p, r, min_p, 0);
                if (s->framelookback && p == FLIF16_PLANE_LOOKBACK && s->max > fr)
                    s->max = fr;
    case 4:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SET(s, fr, p, r, s->c, curr);
            }
        } // End if

        // If this is not the first or only frame, fill the end of the row after
        // the actual pixel data
        if (fr > 0) {
            if (s->alphazero && p < 3) {
                for (uint32_t c = s->end; c < s->width; c++)
                    if (PIXEL_GET(s, fr, 3, r, s->c) == 0) {
                        PIXEL_SET(s, fr, p, r, s->c, flif16_ni_predict(s, fr, p, r));
                    } else {
                        PIXEL_SET(s, fr, p, r, s->c, PIXEL_GET(s, PREV_FRAMENUM(s->frames, fr), p, r, s->c));
                    }
            } else if(p != 4) {
                ff_flif16_copy_cols(CTX_CAST(s), &s->frames[fr],
                PREV_FRAME(s->frames, fr), p, r, s->end, s->width);
            }
        }
    }

    s->segment2 = 0;
    return 0;

need_more_data:
    return AVERROR(EAGAIN);
}

static int flif16_read_ni_image(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    int ret;

    switch (s->segment) {
    case 0:
        for (int p = 0; p < s->range->num_planes; p++)
            s->grays[p] = (ff_flif16_ranges_min(s->range, p) + ff_flif16_ranges_max(s->range, p)) / 2;
        s->i = s->i2 = s->i3 = 0;
        if (   (s->range->num_planes > 3 && ff_flif16_ranges_max(s->range, 3) == 0)
            || (s->range->num_planes > 3 && ff_flif16_ranges_min(s->range, 3) > 0))
            s->alphazero = 0;

        s->segment++;

        for (; s->i < 5; s->i++) {
            s->curr_plane = plane_ordering[s->i];

            if (s->curr_plane >= s->num_planes)
                continue;

            if (IS_CONSTANT(s->range, s->curr_plane))
                continue;

            for (; s->i2 < s->height; s->i2++) {
                for (; s->i3 < s->num_frames; s->i3++) {
    case 1:
                    ret = flif16_read_ni_plane_row(s, s->curr_plane, s->i3, s->i2);
                    if (ret)
                        goto error;
                } // End for
                s->i3 = 0;
            } // End for
            s->i2 = 0;
        } // End for
    } // End switch

    s->state = FLIF16_OUTPUT;
    return 0;

error:
    return ret;
}

/*
 * ============================================================================
 * Interlaced plane decoding
 * ============================================================================
 *
 * This is how the data is organized here:
 * 1. uni_int: rough_zoomlevel
 * 2. (repeat num_planes times) values of top left pixels of each channel
 * 3. Rough Pixeldata max_zoomlevel to rough_zoomlevel + 1
 *    For this case, the MANIAC forest is initialised with a single node per
 *    channel. This is nused with the maniac integer reader.
 * 4. Actual Encoded MANIAC trees
 * 5. Rest of the pixeldata rough_zoomlevel to 0
 */

static inline FLIF16ColorVal flif16_predict_horiz(FLIF16DecoderContext *s,
                                                  uint32_t fr, uint8_t z,
                                                  uint8_t p, uint32_t r,
                                                  uint32_t rows)
{
    FLIF16ColorVal top, bottom, avg, left, topleft, bottomleft;
    if (p == FLIF16_PLANE_LOOKBACK)
        return 0;
    top    = PIXEL_GETZ(s, fr, p, z, r - 1, s->c);
    bottom = (r + 1 < rows ? PIXEL_GETZ(s, fr, p, z, r + 1, s->c) : top);

    switch (s->ipp) {
    case 0:
        avg = (top + bottom)>>1;
        return avg;

    case 1:
        avg        = (top + bottom) >> 1;
        left       = (s->c > 0 ? PIXEL_GETZ(s, fr, p, z, r, s->c - 1) : top);
        topleft    = (s->c > 0 ? PIXEL_GETZ(s, fr, p, z, r - 1, s->c - 1) : top);
        bottomleft = (s->c > 0 && r + 1 < rows ? PIXEL_GETZ(s, fr, p, z, r + 1, s->c - 1) : left);
        return MEDIAN3(avg, (FLIF16ColorVal) (left + top - topleft), (FLIF16ColorVal) (left + bottom - bottomleft));

    default:
        left = (s->c > 0 ? PIXEL_GETZ(s, fr, p, z, r, s->c - 1) : top);
        return MEDIAN3(top, bottom, left);
    }
}

static inline FLIF16ColorVal flif16_predict_vert(FLIF16DecoderContext *s,
                                                 uint32_t fr, uint8_t z,
                                                 uint8_t p, uint32_t r,
                                                 uint32_t cols)
{
    FLIF16ColorVal top, left, right, avg, topleft, topright;
    if (p == FLIF16_PLANE_LOOKBACK)
        return 0;
    left  = PIXEL_GETZ(s, fr, p, z, r, s->c - 1);
    right = (s->c + 1 < cols ? PIXEL_GETZ(s, fr, p, z, r, s->c + 1) : left);

    switch (s->ipp) {
    case 0:
        avg = (left + right) >> 1;
        return avg;

    case 1:
        avg      = (left + right) >> 1;
        top      = (r > 0 ? PIXEL_GETZ(s, fr, p, z, r - 1, s->c) : left);
        topleft  = (r > 0 ? PIXEL_GETZ(s, fr, p, z , r - 1, s->c - 1) : left);
        topright = (r > 0 && s->c + 1 < cols ? PIXEL_GETZ(s, fr, p, z, r - 1, s->c + 1) : top);
        return MEDIAN3(avg, (FLIF16ColorVal) (left + top - topleft), (FLIF16ColorVal) (right + top - topright));

    default:
        top = (r > 0 ? PIXEL_GETZ(s, fr, p, z, r - 1, s->c) : left);
        return MEDIAN3(top, left, right);
    }
}

static inline FLIF16ColorVal flif16_predict_calcprops(FLIF16DecoderContext *s,
                                                      uint32_t fr, int8_t z,
                                                      uint8_t p, uint32_t r,
                                                      uint8_t horizontal,
                                                      uint8_t nobordercases)
{
    FLIF16ColorVal guess, left, top, topleft, topright, bottomleft, bottom,
                   avg, topleftgradient, median, bottomright, right;
    const uint8_t bottompresent = r + 1 < ZOOM_HEIGHT(s->height, z);
    const uint8_t rightpresent  = s->c + 1 < ZOOM_WIDTH(s->width, z);
    int which = 0;
    int index = 0;

    if (p < 3) {
        if (p > 0) {
            s->properties[index++] = PIXEL_GETFAST(s, fr, FLIF16_PLANE_Y, r, s->c);
        }
        if (p > 1)
            s->properties[index++] = PIXEL_GETZ(s, fr, FLIF16_PLANE_CO, z, r, s->c);
        if (s->num_planes > 3)
            s->properties[index++] = PIXEL_GETZ(s, fr, FLIF16_PLANE_ALPHA, z, r, s->c);
    }

    if (horizontal) { // filling horizontal lines
        top        = PIXEL_GETFAST(s, fr, p, r - 1, s->c);
        left       =   (nobordercases || s->c > 0
                     ? PIXEL_GETFAST(s, fr, p, r, s->c - 1)
                     : top);
        topleft    =   (nobordercases || s->c > 0
                     ? PIXEL_GETFAST(s, fr, p, r - 1, s->c - 1)
                     : top);
        topright   =   (nobordercases || (rightpresent)
                     ? PIXEL_GETFAST(s, fr, p, r - 1, s->c + 1)
                     : top);
        bottomleft =   (nobordercases || (bottompresent && s->c > 0)
                     ? PIXEL_GETFAST(s, fr, p, r + 1, s->c - 1)
                     : left);
        bottom     =   (nobordercases || bottompresent
                     ? PIXEL_GETFAST(s, fr, p, r + 1, s->c)
                     : left);
        avg             = (top + bottom) >> 1;
        topleftgradient = left + top - topleft;
        median          = MEDIAN3(avg, topleftgradient, (FLIF16ColorVal) (left + bottom - bottomleft));
        which = 2;

        if (median == avg)
            which = 0;
        else if (median == topleftgradient)
            which = 1;
        s->properties[index++] = which;

        if (p == FLIF16_PLANE_CO || p == FLIF16_PLANE_CG) {
            s->properties[index++] = PIXEL_GETFAST(s, fr, FLIF16_PLANE_Y, r, s->c) -
                                     ((PIXEL_GETFAST(s, fr, FLIF16_PLANE_Y, r - 1, s->c) +
                                     PIXEL_GETFAST(s, fr, FLIF16_PLANE_Y,
                                     (nobordercases || bottompresent ? r + 1 : r - 1), s->c)) >> 1);
        }

        switch (s->predictor) {
        case 0:
            guess = avg;
            break;
        case 1:
            guess = median;
            break;
        default:
            guess = MEDIAN3(top, bottom, left);
            break;
        }

        ff_flif16_ranges_snap(s->range, p, s->properties, &s->min, &s->max, &guess);
        s->properties[index++] = top - bottom;
        s->properties[index++] = top - ((topleft + topright) >> 1);
        s->properties[index++] = left - ((bottomleft + topleft) >> 1);
        bottomright =    (nobordercases || (rightpresent && bottompresent)
                       ? PIXEL_GETFAST(s, fr, p, r + 1, s->c + 1)
                       : bottom);
        s->properties[index++] = bottom - ((bottomleft + bottomright) >> 1);
    } else { // filling vertical lines
        left       = PIXEL_GETFAST(s, fr, p, r, s->c - 1);
        top        =   (nobordercases || r > 0
                     ? PIXEL_GETFAST(s, fr, p, r - 1, s->c)
                     : left);
        topleft    =   (nobordercases || r > 0
                     ? PIXEL_GETFAST(s, fr, p, r - 1, s->c - 1)
                     : left);
        topright   =   (nobordercases || (r > 0 && rightpresent)
                     ? PIXEL_GETFAST(s, fr, p, r - 1, s->c + 1)
                     : top);
        bottomleft =   (nobordercases || (bottompresent)
                     ? PIXEL_GETFAST(s, fr, p, r + 1, s->c - 1)
                     : left);
        right      =   (nobordercases || rightpresent
                     ? PIXEL_GETFAST(s, fr, p, r, s->c + 1)
                     : top);
        avg        = (left + right) >> 1;
        topleftgradient = left + top - topleft;
        median = MEDIAN3(avg, topleftgradient, (FLIF16ColorVal) (right + top - topright));
        which = 2;

        if (median == avg)
            which = 0;
        else if (median == topleftgradient)
            which = 1;

        s->properties[index++] = which;

        if (p == FLIF16_PLANE_CO || p == FLIF16_PLANE_CG) {
            s->properties[index++] = PIXEL_GETFAST(s, fr, FLIF16_PLANE_Y, r, s->c) -
                                     ((PIXEL_GETFAST(s, fr, FLIF16_PLANE_Y, r, s->c - 1) +
                                     PIXEL_GETFAST(s, fr, FLIF16_PLANE_Y, r,
                                     (nobordercases || rightpresent ? s->c + 1 : s->c - 1))) >> 1);
        }

        switch (s->predictor) {
        case 0:
            guess = avg;
            break;
        case 1:
            guess = median;
            break;
        default:
            guess = MEDIAN3(top, left, right);
            break;
        }

        ff_flif16_ranges_snap(s->range, p, s->properties, &s->min, &s->max, &guess);
        s->properties[index++] = left - right;
        s->properties[index++] = left - ((bottomleft + topleft) >> 1);
        s->properties[index++] = top  - ((topleft + topright) >> 1);
        bottomright =   (nobordercases || (rightpresent && bottompresent)
                      ? PIXEL_GETFAST(s, fr, p, r + 1, s->c + 1)
                      : right);
        s->properties[index++] = right - ((bottomright + topright) >> 1);
    }

    s->properties[index++] = guess;

    if (p != 2) {
        if (nobordercases || r > 1)
            s->properties[index++] = PIXEL_GETFAST(s, fr, p, r - 2, s->c) - top; // toptop - top
        else
            s->properties[index++] = 0;
        if (nobordercases || s->c > 1)
            s->properties[index++] = PIXEL_GETFAST(s, fr, p, r, s->c - 2) - left; // leftleft - left
        else
            s->properties[index++] = 0;
    }

    return guess;
}

static int flif_read_plane_zl_horiz(FLIF16DecoderContext *s,
                                    uint8_t alpha_plane, int p,
                                    int z, uint32_t fr, uint32_t r)
{
    FLIF16ColorVal curr;
    const uint32_t cs = ZOOM_COLPIXELSIZE(z), rs = ZOOM_ROWPIXELSIZE(z);
    const uint32_t zh = ZOOM_HEIGHT(s->height, z), zw = ZOOM_WIDTH(s->width, z);

    switch (s->segment2) {
    case 0:
        if (s->frames[fr].seen_before >= 0) {
            return 0;
        }

        if (fr > 0) {
            s->begin = s->frames[fr].col_begin[r * rs] / cs;
            s->end   = 1 + (s->frames[fr].col_end[r * rs] - 1) / cs;
            if (s->alphazero && p < 3) {
                for (s->c = 0; s->c < s->begin; s->c++)
                    if (PIXEL_GETZ(s, fr, FLIF16_PLANE_ALPHA, z, r, s->c) == 0)
                        PIXEL_SETZ(s, fr, p,  z, r, s->c,
                                   flif16_predict_horiz(s, fr, z, p, r, zh));
                    else
                        PIXEL_SETZ(s, fr, p, z, r, s->c,
                                   PIXEL_GETZ(s, fr - 1, p, z, r, s->c));
            } else if (p != 4) {
                ff_flif16_copy_cols_stride(CTX_CAST(s), &s->frames[fr],
                                           &s->frames[fr - 1], p,
                                           rs * r, cs * 0, cs * s->begin, cs);
                ff_flif16_copy_cols_stride(CTX_CAST(s), &s->frames[fr],
                                           &s->frames[fr - 1], p,
                                           rs * r, cs * s->end,
                                           cs * zw, cs);
            }
        } else {
            s->begin = 0;
            s->end   = zw;
        }

        s->segment2++;

        if (r > 1 && r < zh - 1 && !s->framelookback && s->begin == 0 && s->end > 3) {
            for (s->c = s->begin; s->c < 2; s->c++) {
                if (s->alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  flif16_predict_horiz(s, fr, z, p, r, zh));
                    continue;
                }
                s->guess = flif16_predict_calcprops(s, fr, z, p, r, 1, 0);
    case 1:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SETFAST(s, fr, p, r, s->c, curr);
            }
            s->segment2++;

            for (s->c = 2; s->c < s->end - 2; s->c++) {
                if (s->alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  flif16_predict_horiz(s, fr, z, p, r, zh));
                    continue;
                }
                s->guess = flif16_predict_calcprops(s, fr, z, p, r, 1, 1);
    case 2:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SETFAST(s, fr, p, r, s->c, curr);
            }
            s->segment2++;

            for (s->c = s->end - 2; s->c < s->end; s->c++) {
                if (s->alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  flif16_predict_horiz(s, fr, z, p, r, zh));
                    continue;
                }
                s->guess = flif16_predict_calcprops(s, fr, z, p, r, 1, 0);
    case 3:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SETFAST(s, fr, p, r, s->c, curr);
            }
        } else {
            s->segment2 = 4;
            for (s->c = s->begin; s->c < s->end; s->c++) {
                if (s->alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  flif16_predict_horiz(s, fr, z, p, r, zh));
                    continue;
                }
                if (s->framelookback && p < 4 && PIXEL_GETZ(s, fr, FLIF16_PLANE_LOOKBACK, z, r, s->c) > 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  PIXEL_GETZ(s, LOOKBACK_FRAMENUMZ(s, s->frames, fr, z, r, s->c),
                                  p, z, r, s->c));
                    continue;
                }

                s->guess = flif16_predict_calcprops(s, fr, z, p, r, 1, 0);

                if (s->framelookback && p == FLIF16_PLANE_LOOKBACK && s->max > fr)
                    s->max = fr;
                if (s->framelookback && (s->guess > s->max || s->guess < s->min))
                    s->guess = s->min;
    case 4:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SETFAST(s, fr, p, r, s->c, curr);
            }
        }

        if (fr>0 && s->alphazero && p < 3) {
            for (uint32_t c = s->end; c < zw; c++)
                if (PIXEL_GETZ(s, fr, p, z, r, s->c) == 0)
                    PIXEL_SETZ(s, fr, p, z, r, s->c,
                               flif16_predict_horiz(s, fr, z, p, r, zh));
                else
                    PIXEL_SETZ(s, fr, p, z, r, s->c, PIXEL_GETZ(s, fr - 1, p, z, r, s->c));
        }
    }

    s->segment2 = 0;
    return 0;

need_more_data:
    return AVERROR(EAGAIN);
}

static int flif16_read_plane_zl_vert(FLIF16DecoderContext *s,
                                     uint8_t alpha_plane, int p,
                                     int z, uint32_t fr, uint32_t r)
{
    FLIF16ColorVal curr;

    const uint32_t cs = ZOOM_COLPIXELSIZE(z), rs = ZOOM_ROWPIXELSIZE(z);
    const uint32_t zh = ZOOM_HEIGHT(s->height, z), zw = ZOOM_WIDTH(s->width, z);

    switch (s->segment2) {
    case 0:
        if (s->frames[fr].seen_before >= 0) {
            return 0;
        }
        if (fr > 0) {
            s->begin = (s->frames[fr].col_begin[r * rs] / cs);
            s->end = (1 + (s->frames[fr].col_end[r * rs] - 1) / cs) | 1;
            if (s->begin > 1 && ((s->begin & 1) == 0))
                s->begin--;
            if (s->begin == 0)
                s->begin = 1;
            if (s->alphazero && p < 3) {
                for (s->c = 1; s->c < s->begin; s->c += 2)
                    if (PIXEL_GETZ(s, fr, alpha_plane, z, r, s->c) == 0)
                        PIXEL_SETZ(s, fr, p, z, r, s->c,
                                   flif16_predict_vert(s, fr, z, p, r, zw));
                    else
                        PIXEL_SETZ(s, fr, p, z, r, s->c, PIXEL_GETZ(s, fr - 1, p, z, r, s->c));
            } else if (p != 4) {
                ff_flif16_copy_cols_stride(CTX_CAST(s), &s->frames[fr],
                                           &s->frames[fr - 1], p,
                                           rs * r, cs * 1, cs * s->begin, cs * 2);
                ff_flif16_copy_cols_stride(CTX_CAST(s), &s->frames[fr],
                                           &s->frames[fr - 1], p,
                                           rs * r, cs * s->end, cs * zw, cs * 2);
            }
        } else {
            s->begin = 1;
            s->end = zw;
        }
        s->segment2++;

        if (r > 1 && r < zh - 1 && !s->framelookback
            && s->end == zw && s->end > 5 && s->begin == 1) {
            s->c = s->begin;
            for (; s->c < 3; s->c += 2) {
                if (s->alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  flif16_predict_vert(s, fr, z, p, r, zw));
                    continue;
                }
                s->guess = flif16_predict_calcprops(s, fr, z, p, r, 0, 0);
    case 1:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SETFAST(s, fr, p, r, s->c, curr);
            }
            s->segment2++;

            for (; s->c < s->end - 2; s->c += 2) {
                if (s->alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  flif16_predict_vert(s, fr, z, p, r, zw));
                    continue;
                }
                s->guess = flif16_predict_calcprops(s, fr, z, p, r, 0, 1);
    case 2:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SETFAST(s, fr, p, r, s->c, curr);
            }
            s->segment2++;

            for (; s->c < s->end; s->c += 2) {
                if (s->alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  flif16_predict_vert(s, fr, z, p, r, zw));
                    continue;
                }
                s->guess = flif16_predict_calcprops(s, fr, z, p, r, 0, 0);
    case 3:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SETFAST(s, fr, p, r, s->c, curr);
            }
        } else {
            s->segment2 = 4;
            for (s->c = s->begin; s->c < s->end; s->c += 2) {
                if (s->alphazero && p < 3 && PIXEL_GETFAST(s, fr, alpha_plane, r, s->c) == 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                  flif16_predict_vert(s, fr, z, p, r, zw));
                    continue;
                }
                if (s->framelookback && p < 4
                     && PIXEL_GETZ(s, fr, FLIF16_PLANE_LOOKBACK, z, r, s->c) > 0) {
                    PIXEL_SETFAST(s, fr, p, r, s->c,
                                 PIXEL_GETZ(s, LOOKBACK_FRAMENUMZ(s, s->frames,
                                 fr, z, r, s->c), p, z, r, s->c));
                    continue;
                }
                s->guess = flif16_predict_calcprops(s, fr, z, p, r, 0, 0);
                if (s->framelookback && p == FLIF16_PLANE_LOOKBACK && s->max > fr)
                    s->max = fr;
                if (s->framelookback && (s->guess > s->max || s->guess < s->min))
                    s->guess = s->min;
    case 4:
                MANIAC_GET(&s->rc, &s->maniac_ctx, s->properties, p,
                           s->min - s->guess, s->max - s->guess, &curr);
                curr += s->guess;
                PIXEL_SETFAST(s, fr, p, r, s->c, curr);
            }
        }
    }

    if (fr > 0 && s->alphazero && p < 3) {
        for (s->c = s->end; s->c < zw; s->c += 2)
            if (PIXEL_GETZ(s, fr - 1, alpha_plane, z, r, s->c) == 0)
                PIXEL_SETZ(s, fr, p, z, r, s->c,
                           flif16_predict_vert(s, fr, z, p, r, zw));
            else
                PIXEL_SETZ(s, fr, p, z, r, s->c, PIXEL_GETZ(s, fr - 1, p, z, r, s->c));
    }


    s->segment2 = 0;
    return 0;

need_more_data:
    return AVERROR(EAGAIN);

}

static inline int plane_zoomlevels(uint8_t num_planes, int begin_zl, int end_zl)
{
    return num_planes * (begin_zl - end_zl + 1);
}

static inline int get_plane_zoomlevel(uint8_t num_planes, int begin_zl, int end_zl,
                                      int i, FLIF16RangesContext *ranges)
{
    int zl_list[MAX_PLANES] = {0};
    int nextp, highest_priority_plane = 0;

    /*
     * From reference decoder:
     *
     * more advanced order: give priority to more important plane(s)
     * assumption: plane 0 is luma, plane 1 is chroma, plane 2 is less important
     * chroma, plane 3 is perhaps alpha, plane 4 are frame lookbacks (lookback
     * transform, animation only)
     */

    int max_behind[] = {0, 2, 4, 0, 0};

    if (IS_CONSTANT(ranges, 0)) {
        max_behind[1] = 0;
        max_behind[2] = 1;
    }

    for (int i = 0; i < num_planes; i++)
        zl_list[i] = begin_zl + 1;

    if (num_planes >= 5)
        highest_priority_plane = 4; // lookbacks first
    else if (num_planes >= 4)
        highest_priority_plane = 3; // alpha first

    nextp = highest_priority_plane;

    while (i >= 0) {
        zl_list[nextp]--;
        i--;
        if (i < 0)
            break;
        nextp = highest_priority_plane;
        for (int p = 0; p < num_planes; p++) {
            if (zl_list[p] > zl_list[highest_priority_plane] + max_behind[p]) {
                nextp = p;
            }
        }

        // ensure that nextp is not at the most detailed zoomlevel yet
        while (zl_list[nextp] <= end_zl)
            nextp = (nextp + 1) % num_planes;
    }

    return nextp;
}

static int flif16_read_image(AVCodecContext *avctx, uint8_t rough) {
    FLIF16DecoderContext *s = avctx->priv_data;
    int ret;
    int temp;
    uint8_t nump = s->num_planes;
    uint8_t alpha_plane = (s->num_planes > 3) ? 3 : 0;

    if (!rough && !s->segment) { // Are we decoding the main pixeldata segment?
        s->begin_zl = s->rough_zl;
        s->end_zl = 0;
        s->segment = 5;
    }

    switch (s->segment) {
    case 0:
        flif16_blank_maniac_forest_init(avctx);
        s->segment++;

    case 1:
        s->begin_zl = 0;
        while (   ZOOM_ROWPIXELSIZE(s->begin_zl) < s->height
               || ZOOM_COLPIXELSIZE(s->begin_zl) < s->width)
            s->begin_zl++;
        s->segment++;

    case 2:
        RAC_GET(&s->rc, NULL, 0, s->begin_zl, &s->rough_zl, FLIF16_RAC_UNI_INT32);
        s->end_zl = s->rough_zl + 1;
        s->segment++;

        // Read top left pixels of all planes
        s->i = 0;
        s->i2 = 0;
        for (; s->i < s->num_planes; s->i++) {
            if (!IS_CONSTANT(s->range, s->i)) {
                for (; s->i2 < s->num_frames; s->i2++) {
    case 3:
                    RAC_GET(&s->rc, NULL, ff_flif16_ranges_min(s->range, s->i),
                            ff_flif16_ranges_max(s->range, s->i) - ff_flif16_ranges_min(s->range, s->i),
                            &temp, FLIF16_RAC_UNI_INT32);
                    PIXEL_SETZ(s, s->i2, s->i, 0, 0, 0, temp);
                }
                s->i2 = 0;
            }
        }
        s->segment++;

    case 4:
        for (int i = 0; i < nump; i++)
            s->zoomlevels[i] = s->begin_zl;
        s->segment++;

    /* Inner Segment */
    case 5:
        RAC_GET(&s->rc, NULL, 0, 1, &s->default_order, FLIF16_RAC_UNI_INT8);
        s->segment++;

        for (s->i = 0; s->i < nump; s->i++) {
    case 6:
            RAC_GET(&s->rc, NULL, -1, MAX_PREDICTORS + 1, &s->predictors[s->i], FLIF16_RAC_UNI_INT32);
        }
        s->segment++;

        for (s->i = 0; s->i < plane_zoomlevels(nump, s->begin_zl, s->end_zl); s->i++) {
    case 7:
            if (s->default_order) {
                s->curr_plane = get_plane_zoomlevel(s->num_planes, s->begin_zl,
                                                    s->end_zl, s->i, s->range);
            } else {
                RAC_GET(&s->rc, NULL, 0, nump - 1, &s->curr_plane, FLIF16_RAC_UNI_INT32);
            }
            s->segment++;
            s->curr_zoom = s->zoomlevels[s->curr_plane];

            if (s->curr_zoom < 0) {
                av_log(s, AV_LOG_ERROR, "invalid plane/zoomlevel\n");
                return AVERROR_INVALIDDATA;
            }

            if (!IS_CONSTANT(s->range, s->curr_plane)) {
                if (s->predictors[s->curr_plane] < 0) {
    case 8:
                    RAC_GET(&s->rc, NULL, 0, MAX_PREDICTORS, &s->predictor, FLIF16_RAC_UNI_INT32);
                } else {
                    s->predictor = s->predictors[s->curr_plane];
                }
                s->segment++;

                for(int fr = 0; fr < s->num_frames; fr++) {
                    ff_flif16_prepare_zoomlevel(CTX_CAST(s), &s->frames[fr],
                                                s->curr_plane, s->curr_zoom);
                    if (s->curr_plane > 0)
                        ff_flif16_prepare_zoomlevel(CTX_CAST(s), &s->frames[fr],
                                                    0, s->curr_zoom);
                    if (s->curr_plane < 3 && s->num_planes > 3)
                        ff_flif16_prepare_zoomlevel(CTX_CAST(s), &s->frames[fr],
                                                    3, s->curr_zoom);
                }

                if (!(s->curr_zoom % 2)) {
                    s->segment = 9;
                    for (s->i2 = 1; s->i2 < ZOOM_HEIGHT(s->height, s->curr_zoom); s->i2 += 2) {
                        for (s->i3 = 0; s->i3 < s->num_frames; s->i3++) {
    case 9:
                            if(ret = flif_read_plane_zl_horiz(s, alpha_plane,
                               s->curr_plane, s->curr_zoom, s->i3, s->i2))
                                goto error;
                        }
                    }
                } else {
                    s->segment = 10;
                    for (s->i2 = 0; s->i2 < ZOOM_HEIGHT(s->height, s->curr_zoom); s->i2++) {
                        for (s->i3 = 0; s->i3 < s->num_frames; s->i3++) {
    case 10:
                            if(ret = flif16_read_plane_zl_vert(s, alpha_plane,
                               s->curr_plane, s->curr_zoom, s->i3, s->i2))
                                goto error;
                        }
                    }
                }

                s->zoomlevels[s->curr_plane]--;
            } else
                s->zoomlevels[s->curr_plane]--;
            s->segment = 7;
        } // End For
    } // End Switch

    s->state = FLIF16_OUTPUT;
    s->segment = 0;
    s->segment2 = 0;
    return ret;

need_more_data:
    return AVERROR(EAGAIN);

error:
    return ret;
}

static int flif16_read_pixeldata(AVCodecContext *avctx)
{
    FLIF16DecoderContext *s = avctx->priv_data;
    int ret;
    if((s->ia % 2))
        ret = flif16_read_ni_image(avctx);
    else {
        ret = flif16_read_image(avctx, (s->state == FLIF16_ROUGH_PIXELDATA));
    }

    if(!ret)
        s->state = FLIF16_OUTPUT;

    return ret;
}

static int flif16_write_frame(AVCodecContext *avctx, AVFrame *data)
{
    uint32_t target_frame;
    int ret;
    FLIF16DecoderContext *s = avctx->priv_data;
    s->out_frame->pict_type = AV_PICTURE_TYPE_I;

    if ((ret = ff_set_dimensions(avctx, s->width, s->height)) < 0)
        return ret;

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

    // Clear out transparent pixels
    if (s->num_planes > 3) {
        for (uint32_t i = 0; i < s->height; i++)
            for (uint32_t j = 0; j < s->width; j++)
                if (!PIXEL_GET(s, s->out_frames_count, FLIF16_PLANE_ALPHA, i, j)) {
                    PIXEL_SET(s, s->out_frames_count, FLIF16_PLANE_Y, i, j, 0);
                    PIXEL_SET(s, s->out_frames_count, FLIF16_PLANE_CO, i, j, 0);
                    PIXEL_SET(s, s->out_frames_count, FLIF16_PLANE_CG, i, j, 0);
                }
    }

    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_GRAY8:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                *(s->out_frame->data[0] + i * s->out_frame->linesize[0] + j) = \
                PIXEL_GET(s, target_frame, 0, i, j);
            }
        }
        break;

    case AV_PIX_FMT_RGB24:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
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
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                *((uint32_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 4))
                = (PIXEL_GET(s, target_frame, 3, i, j) << 24) |
                  (PIXEL_GET(s, target_frame, 0, i, j) << 16) |
                  (PIXEL_GET(s, target_frame, 1, i, j) << 8)  |
                   PIXEL_GET(s, target_frame, 2, i, j);
            }
        }
        break;

    case AV_PIX_FMT_GRAY16:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                *((uint16_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 2)) = \
                PIXEL_GET(s, target_frame, 0, i, j);
            }
        }
        break;

    case AV_PIX_FMT_RGB48:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                *((uint16_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 6 + 0)) = \
                PIXEL_GET(s, target_frame, 0, i, j);
                *((uint16_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 6 + 1)) = \
                PIXEL_GET(s, target_frame, 1, i, j);
                *((uint16_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 6 + 2)) = \
                PIXEL_GET(s, target_frame, 2, i, j);
            }
        }

    case AV_PIX_FMT_RGBA64:
        for (uint32_t i = 0; i < s->height; i++) {
            for (uint32_t j = 0; j < s->width; j++) {
                *((uint64_t *) (s->out_frame->data[0] + i * s->out_frame->linesize[0] + j * 8))
                = (((uint64_t) PIXEL_GET(s, target_frame, 3, i, j)) << 48) |
                  (((uint64_t) PIXEL_GET(s, target_frame, 2, i, j)) << 32) |
                  (((uint64_t) PIXEL_GET(s, target_frame, 1, i, j)) << 16) |
                   ((uint64_t) PIXEL_GET(s, target_frame, 0, i, j));
            }
        }
        break;

    default:
        av_log(avctx, AV_LOG_FATAL, "Pixel format %d out of bounds?\n", avctx->pix_fmt);
        return AVERROR_PATCHWELCOME;
    }

    av_frame_ref(data, s->out_frame);
    if ((++s->out_frames_count) >= s->num_frames)
        s->state = FLIF16_EOS;

    return 0;
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

    /*
     * Looping is done to change states in between functions.
     * Function will either exit on AVERROR(EAGAIN) or AVERROR_EOF
     */
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
            ret = flif16_read_pixeldata(avctx);
            if (!ret) {
                ff_flif16_maniac_close(&s->maniac_ctx, s->num_planes,
                                       s->framelookback);
                s->state = FLIF16_MANIAC;
            }
            break;

        case FLIF16_MANIAC:
            ret = flif16_read_maniac_forest(avctx);
            break;

        case FLIF16_PIXELDATA:
            ret = flif16_read_pixeldata(avctx);
            if (!ret) {
                for (int i = 0; i < s->num_frames; i++) {
                    if (s->frames[i].seen_before >= 0)
                        continue;
                    for (int j = s->transform_top - 1; j >= 0; j--) {
                        ff_flif16_transform_reverse(CTX_CAST(s), s->transforms[j],
                                                    &s->frames[i], 1, 1);
                    }
                }
            }
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
    av_freep(&s->framedelay);
    if (s->frames)
        ff_flif16_frames_free(&s->frames, s->num_frames, s->num_planes, s->framelookback);

    for (int i = s->transform_top - 1; i >= 0; i--)
        ff_flif16_transforms_close(s->transforms[i]);

    ff_flif16_maniac_close(&s->maniac_ctx, s->num_planes, s->framelookback);
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
    .decode         = flif16_decode_frame,
    .close          = flif16_decode_end,
    .priv_data_size = sizeof(FLIF16DecoderContext),
    .capabilities   = AV_CODEC_CAP_DELAY,
    .priv_class     = NULL,
};
