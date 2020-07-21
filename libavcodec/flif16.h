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

// Remove these
//#define __PLN__ printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
//#define MSG(fmt, ...) printf("[%s] " fmt, __func__, ##__VA_ARGS__)
//#include <assert.h>
//#define __PLN__ #error remove me
//#define MSG(fmt,...) #error remove me

#define MAX_PLANES 5
#define MAX_PREDICTORS 2

#define VARINT_APPEND(a,x) (a) = ((a) << 7) | (uint32_t) ((x) & 127)
#define ZOOM_ROWPIXELSIZE(zoomlevel) (1 << (((zoomlevel) + 1) / 2))
#define ZOOM_COLPIXELSIZE(zoomlevel) (1 << (((zoomlevel)) / 2))
#define ZOOM_HEIGHT(r, z) ((!z) ? 0 : (1 + ((r) - 1) / ZOOM_ROWPIXELSIZE(z)))
#define ZOOM_WIDTH(w, z) ((!z) ? 0 : (1 + ((w) - 1) / ZOOM_COLPIXELSIZE(z)))
#define MEDIAN3(a, b, c) (((a) < (b)) ? (((b) < (c)) ? (b) : ((a) < (c) ? (c) : (a))) : (((a) < (c)) ? (a) : ((b) < (c) ? (c) : (b))))

static const uint8_t flif16_header[4] = "FLIF";

typedef enum FLIF16Plane {
    FLIF16_PLANE_Y = 0,
    FLIF16_PLANE_CO,
    FLIF16_PLANE_CG,
    FLIF16_PLANE_ALPHA,    // Alpha plane should be here 
    FLIF16_PLANE_LOOKBACK, // Frame lookback
    FLIF16_PLANE_GRAY = 0, // Is this needed?
} FLIF16Plane;

typedef enum FLIF16PixelType {
    FLIF16_PIXEL_CONSTANT    = 0,
    FLIF16_PIXEL_8           = 1,
    FLIF16_PIXEL_16          = 2,
    FLIF16_PIXEL_32          = 4,
    FLIF16_PIXEL_16_UNSIGNED = 2
} FLIF16PixelType;

static int flif16_pixel_types[2][MAX_PLANES] = {
    {
        FLIF16_PIXEL_8,
        FLIF16_PIXEL_16,
        FLIF16_PIXEL_16,
        FLIF16_PIXEL_8,
        FLIF16_PIXEL_8
    },
    {
        FLIF16_PIXEL_16_UNSIGNED,
        FLIF16_PIXEL_32,
        FLIF16_PIXEL_32,
        FLIF16_PIXEL_16_UNSIGNED,
        FLIF16_PIXEL_8
    }
};


// Each FLIF16PixelData Struct will contain a single frame
// This will work similarly to AVFrame.
// **data will carry an array of planes
// Bounds of these planes will be defined by width and height
// If required, linesize[], similar to AVFrame can be defined.
// If Width, height, and number of planes of each frame is Constant, then
// having numplanes, width, height is redundant. Check.

// TODO replace with AVFrame and av_frame_ref.
typedef struct FLIF16PixelData {
    uint8_t constant_alpha;  // Will Remove Shortly
    //uint8_t palette;       // Maybe this flag is not useful. Will delete it later
    int8_t seen_before;
    uint32_t *col_begin;
    uint32_t *col_end;
    int8_t scale;
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
    uint32_t meta;      ///< Size of a meta chunk

    // Primary Header     
    uint8_t  ia;         ///< Is image interlaced or/and animated or not
    uint32_t bpc;        ///< 2 ^ Bytes per channel
    uint8_t  num_planes; ///< Number of planes
    
    uint8_t loops;        ///< Number of times animation loops
    uint16_t *framedelay; ///< Frame delay for each frame
    FLIF16PixelType plane_type[MAX_PLANES];
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

FLIF16PixelData *ff_flif16_frames_init(FLIF16Context *s);

void ff_flif16_frames_free(FLIF16PixelData *frames, uint32_t num_frames,
                           uint32_t num_planes);


static inline void ff_flif16_pixel_set(FLIF16Context *s, FLIF16PixelData *frame,
                                       uint8_t plane, uint32_t row, uint32_t col,
                                       FLIF16ColorVal value)
{
    //printf("w: plane = %u row = %u col = %u value = %d\n", plane, row, col, value);
    if (s->plane_type[plane])
        ((FLIF16ColorVal *) frame->data[plane])[s->width * row + col] = value;
    else
        ((FLIF16ColorVal *) frame->data[plane])[0] = value;
}

static inline FLIF16ColorVal ff_flif16_pixel_get(FLIF16Context *s,
                                                 FLIF16PixelData *frame,
                                                 uint8_t plane, uint32_t row,
                                                 uint32_t col)
{
    //printf("r: plane = %u row = %u col = %u\n", plane, row, col);
    if (s->plane_type[plane])
        return ((FLIF16ColorVal *) frame->data[plane])[s->width * row + col];
    else
        return ((FLIF16ColorVal *) frame->data[plane])[0];
}


static inline void ff_flif16_pixel_setz(FLIF16Context *s,
                                        FLIF16PixelData *frame,
                                        uint8_t plane, int z, uint32_t row,
                                        uint32_t col, FLIF16ColorVal value)
{
    if (s->plane_type[plane])
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
    if (s->plane_type[plane])
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
    if (s->plane_type[plane])
        return ((FLIF16ColorVal *) frame->data[plane])[row * frame->s_r[plane] + col * frame->s_c[plane]];
    else
        printf("check\n");
}

static inline void ff_flif16_pixel_set_fast(FLIF16Context *s,
                                            FLIF16PixelData *frame,
                                            uint8_t plane, uint32_t row,
                                            uint32_t col, FLIF16ColorVal value)
{
    if (s->plane_type[plane])
        ((FLIF16ColorVal *) frame->data[plane])[row * frame->s_r[plane] + col * frame->s_c[plane]] = value;
    else
        printf("check\n");
}

static inline void ff_flif16_copy_rows(FLIF16Context *s,
                                       FLIF16PixelData *dest,
                                       FLIF16PixelData *src, uint8_t plane,
                                       uint32_t row, uint32_t col_start,
                                       uint32_t col_end)
{
    for(uint32_t col = col_start; col < col_end; ++col) {
        //printf("[%s] col_start = %u col_end = %u plane = %u row = %u\n", __func__, col_start, col_end, plane, row);
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
        //printf("[%s] col_start = %u col_end = %u plane = %u row = %u\n", __func__, col_start, col_end, plane, row);
        ff_flif16_pixel_set(s, dest, plane, row, col, ff_flif16_pixel_get(s, src, plane, row, col));
    }
}
#endif /* AVCODEC_FLIF16_H */
