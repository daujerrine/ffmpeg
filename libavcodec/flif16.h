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
#define MAX_PREDICTORS 2

#define VARINT_APPEND(a,x) (a) = ((a) << 7) | (uint32_t) ((x) & 127)
#define ZOOM_ROWPIXELSIZE(zoomlevel) (1 << (((zoomlevel) + 1) / 2))
#define ZOOM_COLPIXELSIZE(zoomlevel) (1 << (((zoomlevel)) / 2))
#define ZOOM_HEIGHT(r, z) ((!z) ? 0 : (1 + ((r) - 1) / ZOOM_ROWPIXELSIZE(z)))
#define ZOOM_WIDTH(w, z) ((!z) ? 0 : (1 + ((w) - 1) / ZOOM_COLPIXELSIZE(z)))
#define MEDIAN3(a, b, c) (((a) < (b)) ? (((b) < (c)) ? (b) : ((a) < (c) ? (c) : (a))) : (((a) < (c)) ? (a) : ((b) < (c) ? (c) : (b))))

static const uint8_t flif16_header[4] = "FLIF";

// Pixeldata types
static enum AVPixelFormat flif16_out_frame_type[][2] = {
    { -1,  -1 },  // Padding
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
    FLIF16_PLANE_LOOKBACK, // Frame lookback
    FLIF16_PLANE_GRAY = 0, // Is this needed?
} FLIF16Plane;

typedef enum FLIF16PlaneMode {
    FLIF16_PLANEMODE_CONSTANT = 0,  ///< A true constant plane
    FLIF16_PLANEMODE_NORMAL,        ///< A normal pixel matrix    
    FLIF16_PLANEMODE_FILL           /**< A constant plane that is later manipulated
                                         by transforms, making it nonconstant and
                                         allocating a plane for it */
                                    
} FLIF16PlaneMode;

typedef struct FLIF16PixelData {
    int8_t seen_before;  // Required by FrameDup
    uint32_t *col_begin; // Required by FrameShape
    uint32_t *col_end;   // Required by FrameShape
    int s_r[MAX_PLANES];
    int s_c[MAX_PLANES];
    void **data;
} FLIF16PixelData;

typedef int32_t FLIF16ColorVal;

typedef struct FLIF16Context {
    GetByteContext gb;
    FLIF16MANIACContext maniac_ctx;
    FLIF16RangeCoder rc;

    // Dimensions and other things.
    uint32_t width;
    uint32_t height;
    uint32_t num_frames;
    uint32_t meta;        ///< Size of a meta chunk

    // Primary Header     
    uint8_t  ia;          ///< Is image interlaced or/and animated or not
    uint32_t bpc;         ///< 2 ^ Bytes per channel
    uint8_t  num_planes;  ///< Number of planes
    uint8_t loops;        ///< Number of times animation loops
    uint16_t *framedelay; ///< Frame delay for each frame
    uint8_t plane_mode[MAX_PLANES];

    // Transform flags
    uint8_t framedup;
    uint8_t frameshape;
    uint8_t framelookback;
} FLIF16Context;

typedef struct FLIF16RangesContext {
    uint8_t r_no;
    uint8_t num_planes;
    void* priv_data;
} FLIF16RangesContext;

typedef struct FLIF16Ranges {
    uint8_t priv_data_size;

    FLIF16ColorVal (*min)(FLIF16RangesContext *ranges, int plane);
    FLIF16ColorVal (*max)(FLIF16RangesContext *ranges, int plane);
    void (*minmax)(FLIF16RangesContext *ranges, const int plane,
                   FLIF16ColorVal *prev_planes, FLIF16ColorVal *minv,
                   FLIF16ColorVal *maxv);
    void (*snap)(FLIF16RangesContext*, const int, FLIF16ColorVal*,
                 FLIF16ColorVal*, FLIF16ColorVal*, FLIF16ColorVal*);
    uint8_t is_static;
    void (*close)(FLIF16RangesContext*);
    void (*previous)(FLIF16RangesContext*);  //TODO : Maybe remove it later
} FLIF16Ranges;

typedef struct FLIF16TransformContext{
    uint8_t t_no;
    unsigned int segment;     //segment the code is executing in.
    int i;                    //variable to store iteration number.
    uint8_t done;
    void *priv_data;
} FLIF16TransformContext;

typedef struct FLIF16Transform {
    int16_t priv_data_size;
    //Functions
    int8_t (*init) (FLIF16TransformContext *t_ctx, FLIF16RangesContext *r_ctx);
    int8_t (*read) (FLIF16TransformContext *t_ctx, FLIF16Context *ctx,
                    FLIF16RangesContext *r_ctx);
    FLIF16RangesContext *(*meta) (FLIF16Context *ctx,
                                  FLIF16PixelData *frame, uint32_t frame_count,
                                  FLIF16TransformContext *t_ctx,
                                  FLIF16RangesContext *r_ctx);
    int8_t (*forward) (FLIF16Context *ctx, FLIF16TransformContext *t_ctx, FLIF16PixelData *frame);
    int8_t (*reverse) (FLIF16Context *ctx, FLIF16TransformContext *t_ctx, FLIF16PixelData *frame,
                       uint32_t stride_row, uint32_t stride_col);
    void (*configure) (FLIF16TransformContext *, const int);
    void (*close) (FLIF16TransformContext *t_ctx);
} FLIF16Transform;

int32_t (*ff_flif16_maniac_ni_prop_ranges_init(unsigned int *prop_ranges_size,
                                               FLIF16RangesContext *ranges,
                                               uint8_t property,
                                               uint8_t channels))[2];

int32_t (*ff_flif16_maniac_prop_ranges_init(unsigned int *prop_ranges_size,
                                            FLIF16RangesContext *ranges,
                                            uint8_t property,
                                            uint8_t channels))[2];

int ff_flif16_planes_init(FLIF16Context *s, FLIF16PixelData *frames,
                          uint8_t *is_const, uint8_t *const_plane_value);

FLIF16PixelData *ff_flif16_frames_init(FLIF16Context *s);

void ff_flif16_frames_free(FLIF16PixelData **frames, uint32_t num_frames,
                           uint32_t num_planes);



/*
 * All constant plane pixel setting should be illegal in theory.
 */

static inline void ff_flif16_pixel_set(FLIF16Context *s, FLIF16PixelData *frame,
                                       uint8_t plane, uint32_t row, uint32_t col,
                                       FLIF16ColorVal value)
{
    if (s->plane_mode[plane])
        ((FLIF16ColorVal *) frame->data[plane])[s->width * row + col] = value;
    else
        ((FLIF16ColorVal *) frame->data[plane])[0] = value;
}

static inline FLIF16ColorVal ff_flif16_pixel_get(FLIF16Context *s,
                                                 FLIF16PixelData *frame,
                                                 uint8_t plane, uint32_t row,
                                                 uint32_t col)
{
    if (s->plane_mode[plane])
        return ((FLIF16ColorVal *) frame->data[plane])[s->width * row + col];
    else
        return ((FLIF16ColorVal *) frame->data[plane])[0];
}


static inline void ff_flif16_pixel_setz(FLIF16Context *s,
                                        FLIF16PixelData *frame,
                                        uint8_t plane, int z, uint32_t row,
                                        uint32_t col, FLIF16ColorVal value)
{
    if (s->plane_mode[plane])
        ((FLIF16ColorVal *) frame->data[plane])[(row * ZOOM_ROWPIXELSIZE(z)) * s->width +
                                                (col * ZOOM_COLPIXELSIZE(z))] = value;
    else
        ((FLIF16ColorVal *) frame->data[plane])[0] = value;
}

static inline FLIF16ColorVal ff_flif16_pixel_getz(FLIF16Context *s,
                                                  FLIF16PixelData *frame,
                                                  uint8_t plane, int z,
                                                  size_t row, size_t col)
{
    if (s->plane_mode[plane])
        return ((FLIF16ColorVal *) frame->data[plane])[(row * ZOOM_ROWPIXELSIZE(z)) *
                                                       s->width + (col * ZOOM_COLPIXELSIZE(z))];
    else
        return ((FLIF16ColorVal *) frame->data[plane])[0];
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
    if (s->plane_mode[plane])
        return ((FLIF16ColorVal *) frame->data[plane])[row * frame->s_r[plane] + col * frame->s_c[plane]];

    return 0;
}

static inline void ff_flif16_pixel_set_fast(FLIF16Context *s,
                                            FLIF16PixelData *frame,
                                            uint8_t plane, uint32_t row,
                                            uint32_t col, FLIF16ColorVal value)
{
    if (s->plane_mode[plane])
        ((FLIF16ColorVal *) frame->data[plane])[row * frame->s_r[plane] + col * frame->s_c[plane]] = value;
}

static inline void ff_flif16_copy_rows(FLIF16Context *s,
                                       FLIF16PixelData *dest,
                                       FLIF16PixelData *src, uint8_t plane,
                                       uint32_t row, uint32_t col_start,
                                       uint32_t col_end)
{
    for(uint32_t col = col_start; col < col_end; ++col) {
        ff_flif16_pixel_set(s, dest, plane, row, col, ff_flif16_pixel_get(s, src, plane, row, col));
    }
}

static inline void ff_flif16_copy_rows_stride(FLIF16Context *s,
                                              FLIF16PixelData *dest,
                                              FLIF16PixelData *src, uint8_t plane,
                                              uint32_t row, uint32_t col_start,
                                              uint32_t col_end, uint32_t stride)
{
    for(uint32_t col = col_start; col < col_end; col += stride) {
        ff_flif16_pixel_set(s, dest, plane, row, col, ff_flif16_pixel_get(s, src, plane, row, col));
    }
}
#endif /* AVCODEC_FLIF16_H */
