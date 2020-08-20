/*
 * FLIF16 Image Format Definitions
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
 * FLIF16 format definitions and functions.
 */

#ifndef AVCODEC_FLIF16_H
#define AVCODEC_FLIF16_H

#include <stdint.h>
#include <stdlib.h>

#include "avcodec.h"
#include "libavutil/pixfmt.h"
#include "flif16_rangecoder.h"

#define MAX_PLANES 5
#define MAX_TRANSFORMS 13
#define MAX_PROPERTIES 12
#define MAX_PREDICTORS 2
#define MAX_PROP_RANGES 12

#define VARINT_APPEND(a,x) (a) = (((a) << 7) | (uint32_t) ((x) & 127))
#define ZOOM_ROWPIXELSIZE(zoomlevel) (1 << (((zoomlevel) + 1) / 2))
#define ZOOM_COLPIXELSIZE(zoomlevel) (1 << (((zoomlevel)) / 2))
#define ZOOM_HEIGHT(h, z) ((!h) ? 0 : (1 + ((h) - 1) / ZOOM_ROWPIXELSIZE(z)))
#define ZOOM_WIDTH(w, z) ((!w) ? 0 : (1 + ((w) - 1) / ZOOM_COLPIXELSIZE(z)))
#define MEDIAN3(a, b, c) (((a) < (b)) ? (((b) < (c)) ?  (b) : ((a) < (c) ? (c) : (a))) : (((a) < (c)) ? (a) : ((b) < (c) ? (c) : (b))))

static const uint8_t flif16_header[4] = "FLIF";

// Pixeldata types
static const enum AVPixelFormat flif16_out_frame_type[][2] = {
    { -1,  -1 }, // Padding
    { AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY16 },
    { -1 , -1 }, // Padding
    { AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB48  },
    { AV_PIX_FMT_RGB32, AV_PIX_FMT_RGBA64 }
};

typedef enum FLIF16Plane {
    FLIF16_PLANE_Y = 0,
    FLIF16_PLANE_CO,
    FLIF16_PLANE_CG,
    FLIF16_PLANE_ALPHA,
    FLIF16_PLANE_LOOKBACK // Frame lookback
} FLIF16Plane;

typedef enum FLIF16PlaneMode {
    FLIF16_PLANEMODE_CONSTANT = 0, ///< A true constant plane
    FLIF16_PLANEMODE_NORMAL,       ///< A normal pixel matrix
    FLIF16_PLANEMODE_FILL          /**< A constant plane that is later manipulated
                                        by transforms, making it nonconstant and
                                        allocating a plane for it */
} FLIF16PlaneMode;

typedef struct FLIF16PixelData {
    int8_t seen_before;  // Required by FrameDup
    uint32_t *col_begin; // Required by FrameShape
    uint32_t *col_end;   // Required by FrameShape
    int s_r[MAX_PLANES];
    int s_c[MAX_PLANES];
    void *data[MAX_PLANES];
    uint8_t palette;
} FLIF16PixelData;

typedef int32_t FLIF16ColorVal;

typedef struct FLIF16Context {
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
    FLIF16PlaneMode  plane_mode[MAX_PLANES];

    // Transform flags
    uint8_t framedup;
    uint8_t frameshape;
    uint8_t framelookback;
} FLIF16Context;

typedef struct FLIF16RangesContext {
    uint8_t r_no;
    uint8_t num_planes;
    void *priv_data;
} FLIF16RangesContext;

typedef struct FLIF16Ranges {
    uint8_t priv_data_size;
    FLIF16ColorVal (*min)(FLIF16RangesContext *ranges, int plane);
    FLIF16ColorVal (*max)(FLIF16RangesContext *ranges, int plane);
    void (*minmax)(FLIF16RangesContext *ranges, int plane,
                   FLIF16ColorVal *prev_planes, FLIF16ColorVal *minv,
                   FLIF16ColorVal *maxv);
    void (*snap)(FLIF16RangesContext *r_ctx, int plane,
                 FLIF16ColorVal *prev_planes, FLIF16ColorVal *minv,
                 FLIF16ColorVal *maxv, FLIF16ColorVal *v);
    uint8_t is_static;
    void (*close)(FLIF16RangesContext *ctx);
} FLIF16Ranges;

typedef struct FLIF16TransformContext {
    uint8_t t_no;
    unsigned int segment; ///< Segment the code is executing in.
    int i;                ///< Variable to store iteration number.
    void *priv_data;
} FLIF16TransformContext;

typedef struct FLIF16Transform {
    int16_t priv_data_size;
    //Functions
    int (*init) (FLIF16TransformContext *t_ctx, FLIF16RangesContext *r_ctx);
    int (*read) (FLIF16TransformContext *t_ctx, FLIF16Context *ctx,
                 FLIF16RangesContext *r_ctx);
    FLIF16RangesContext *(*meta) (FLIF16Context *ctx, FLIF16PixelData *frame,
                                  uint32_t frame_count, FLIF16TransformContext *t_ctx,
                                  FLIF16RangesContext *r_ctx);
    int (*process) (FLIF16Context *ctx, FLIF16TransformContext *t_ctx,
                    FLIF16RangesContext *src_ctx, FLIF16PixelData *frame);
    void (*forward) (FLIF16Context *ctx, FLIF16TransformContext *t_ctx, FLIF16PixelData *frame);
    void (*write) (FLIF16Context *enc_ctx, FLIF16TransformContext *t_ctx,
                   FLIF16RangesContext *src_ctx);
    void (*reverse) (FLIF16Context *ctx, FLIF16TransformContext *t_ctx, FLIF16PixelData *frame,
                    uint32_t stride_row, uint32_t stride_col);
    void (*configure) (FLIF16TransformContext *ctx, const int setting);
    void (*close) (FLIF16TransformContext *t_ctx);
} FLIF16Transform;

void ff_flif16_maniac_ni_prop_ranges_init(FLIF16MinMax *prop_ranges,
                                          unsigned int *prop_ranges_size,
                                          FLIF16RangesContext *ranges,
                                          uint8_t plane,
                                          uint8_t channels);

void ff_flif16_maniac_prop_ranges_init(FLIF16MinMax *prop_ranges,
                                       unsigned int *prop_ranges_size,
                                       FLIF16RangesContext *ranges,
                                       uint8_t plane,
                                       uint8_t channels);

int ff_flif16_planes_init(FLIF16Context *s, FLIF16PixelData *frame,
                          int32_t *const_plane_value);

FLIF16PixelData *ff_flif16_frames_init(uint16_t num_frames);

FLIF16PixelData *ff_flif16_frames_resize(FLIF16PixelData *frames,
                                         uint16_t curr_num_frames,
                                         uint16_t new_num_frames);

void ff_flif16_frames_free(FLIF16PixelData **frames, uint32_t num_frames,
                           uint32_t num_planes, uint8_t lookback);



/*
 * All constant plane pixel setting should be illegal in theory.
 */

static inline void ff_flif16_pixel_set(FLIF16Context *s, FLIF16PixelData *frame,
                                       uint8_t plane, uint32_t row, uint32_t col,
                                       FLIF16ColorVal value)
{
    ((FLIF16ColorVal *) frame->data[plane])[s->width * row + col] = value;
}

static inline FLIF16ColorVal ff_flif16_pixel_get(FLIF16Context *s,
                                                 FLIF16PixelData *frame,
                                                 uint8_t plane, uint32_t row,
                                                 uint32_t col)
{
    if (s->plane_mode[plane]) {
        return ((FLIF16ColorVal *) frame->data[plane])[s->width * row + col];
    } else
        return ((FLIF16ColorVal *) frame->data[plane])[0];
}


static inline void ff_flif16_pixel_setz(FLIF16Context *s,
                                        FLIF16PixelData *frame,
                                        uint8_t plane, int z, uint32_t row,
                                        uint32_t col, FLIF16ColorVal value)
{
    ((FLIF16ColorVal *) frame->data[plane])[(row * ZOOM_ROWPIXELSIZE(z)) * s->width +
                                            (col * ZOOM_COLPIXELSIZE(z))] = value;
}

static inline FLIF16ColorVal ff_flif16_pixel_getz(FLIF16Context *s,
                                                  FLIF16PixelData *frame,
                                                  uint8_t plane, int z,
                                                  size_t row, size_t col)
{
    if (s->plane_mode[plane]) {
        return ((FLIF16ColorVal *) frame->data[plane])[(row * ZOOM_ROWPIXELSIZE(z)) *
                                                        s->width + (col * ZOOM_COLPIXELSIZE(z))];
    } else {
        return ((FLIF16ColorVal *) frame->data[plane])[0];
    }
}

static inline void ff_flif16_prepare_zoomlevel(FLIF16Context *s,
                                               FLIF16PixelData *frame,
                                               uint8_t plane, int z)
{
    frame->s_r[plane] = ZOOM_ROWPIXELSIZE(z) * s->width;
    frame->s_c[plane] = ZOOM_COLPIXELSIZE(z);
}

static inline FLIF16ColorVal ff_flif16_pixel_get_fast(FLIF16Context *s,
                                                      FLIF16PixelData *frame,
                                                      uint8_t plane, uint32_t row,
                                                      uint32_t col)
{
    if (s->plane_mode[plane]) {
        return ((FLIF16ColorVal *) frame->data[plane])[row * frame->s_r[plane] + col * frame->s_c[plane]];
    } else
        return ((FLIF16ColorVal *) frame->data[plane])[0];
    return 0;
}

static inline void ff_flif16_pixel_set_fast(FLIF16Context *s,
                                            FLIF16PixelData *frame,
                                            uint8_t plane, uint32_t row,
                                            uint32_t col, FLIF16ColorVal value)
{
    ((FLIF16ColorVal *) frame->data[plane])[row * frame->s_r[plane] + col * frame->s_c[plane]] = value;
}

static inline void ff_flif16_copy_cols(FLIF16Context *s,
                                       FLIF16PixelData *dest,
                                       FLIF16PixelData *src, uint8_t plane,
                                       uint32_t row, uint32_t col_start,
                                       uint32_t col_end)
{
    FLIF16ColorVal *desti = ((FLIF16ColorVal *) dest->data[plane]) +
                            s->width * row + col_start;
    FLIF16ColorVal *destif = ((FLIF16ColorVal *) dest->data[plane]) +
                             s->width * row + col_end;
    FLIF16ColorVal *srci = ((FLIF16ColorVal *) src->data[plane]) +
                            s->width * row + col_start;
    while (desti < destif) {
        *(desti++) = *(srci++);
    } 
}

static inline void ff_flif16_copy_cols_stride(FLIF16Context *s,
                                              FLIF16PixelData *dest,
                                              FLIF16PixelData *src, uint8_t plane,
                                              uint32_t row, uint32_t col_start,
                                              uint32_t col_end, uint32_t stride)
{
    for (uint32_t col = col_start; col < col_end; col += stride) {
        ((FLIF16ColorVal *) dest->data[plane])[s->width * row + col] =
        ((FLIF16ColorVal *) src->data[plane])[s->width * row + col];
    }
}
#endif /* AVCODEC_FLIF16_H */
