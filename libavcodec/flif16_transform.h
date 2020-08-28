/*
 * Transforms for FLIF16
 * Copyright (c) 2020 Kartik K. Khullar <kartikkhullar840@gmail.com>
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
 * Transforms for FLIF16.
 */

#ifndef AVCODEC_FLIF16_TRANSFORM_H
#define AVCODEC_FLIF16_TRANSFORM_H

#include "avcodec.h"
#include "libavutil/common.h"
#include "flif16.h"

typedef enum FLIF16RangesTypes {
    FLIF16_RANGES_CHANNELCOMPACT,
    FLIF16_RANGES_YCOCG,
    FLIF16_RANGES_PERMUTEPLANES,
    FLIF16_RANGES_PERMUTEPLANESSUBTRACT,
    FLIF16_RANGES_BOUNDS,
    FLIF16_RANGES_STATIC,
    FLIF16_RANGES_PALETTEALPHA,
    FLIF16_RANGES_PALETTE,
    FLIF16_RANGES_COLORBUCKETS,
    FLIF16_RANGES_FRAMELOOKBACK
} FLIF16RangesTypes;

typedef enum FLIF16TransformsTypes {
    FLIF16_TRANSFORM_CHANNELCOMPACT,
    FLIF16_TRANSFORM_YCOCG,
    FLIF16_TRANSFORM_RESERVED1,
    FLIF16_TRANSFORM_PERMUTEPLANES,
    FLIF16_TRANSFORM_BOUNDS,
    FLIF16_TRANSFORM_PALETTEALPHA,
    FLIF16_TRANSFORM_PALETTE,
    FLIF16_TRANSFORM_COLORBUCKETS,
    FLIF16_TRANSFORM_RESERVED2,
    FLIF16_TRANSFORM_RESERVED3,
    FLIF16_TRANSFORM_DUPLICATEFRAME,
    FLIF16_TRANSFORM_FRAMESHAPE,
    FLIF16_TRANSFORM_FRAMELOOKBACK,
} FLIF16TransformsTypes;

extern const FLIF16Ranges *flif16_ranges[14];
extern const FLIF16Transform *flif16_transforms[MAX_TRANSFORMS];

FLIF16RangesContext *ff_flif16_ranges_static_init(uint8_t num_planes,
                                                  uint32_t bpc);

void ff_flif16_ranges_close(FLIF16RangesContext* r_ctx);

static inline FLIF16ColorVal ff_flif16_ranges_min(FLIF16RangesContext *r_ctx, int p)
{
    const FLIF16Ranges *ranges = flif16_ranges[r_ctx->r_no];
    if (ranges->min)
        return ranges->min(r_ctx, p);
    else
        return 0;
}

static inline FLIF16ColorVal ff_flif16_ranges_max(FLIF16RangesContext *r_ctx, int p)
{
    const FLIF16Ranges *ranges = flif16_ranges[r_ctx->r_no];
    if (ranges->max)
        return ranges->max(r_ctx, p);
    else
        return 0;
}

static inline void ff_flif16_ranges_minmax(FLIF16RangesContext *r_ctx, int p,
                                           FLIF16ColorVal *prev_planes,
                                           FLIF16ColorVal *minv, FLIF16ColorVal *maxv)
{
    flif16_ranges[r_ctx->r_no]->minmax(r_ctx, p, prev_planes, minv, maxv);
}

static inline void ff_flif16_ranges_snap(FLIF16RangesContext *r_ctx, int p,
                                         FLIF16ColorVal *prev_planes, FLIF16ColorVal *minv,
                                         FLIF16ColorVal *maxv, FLIF16ColorVal *v)
{
    flif16_ranges[r_ctx->r_no]->snap(r_ctx, p, prev_planes, minv, maxv, v);
}

FLIF16TransformContext *ff_flif16_transform_init(int t_no,
                                                 FLIF16RangesContext *ranges);

void ff_flif16_transform_configure(FLIF16TransformContext *t_ctx,
                                   const int setting);

int ff_flif16_transform_read(FLIF16Context *ctx, FLIF16TransformContext *t_ctx,
                             FLIF16RangesContext *ranges);

FLIF16RangesContext* ff_flif16_transform_meta(FLIF16Context *ctx,
                                              FLIF16PixelData *frame,
                                              uint32_t frame_count,
                                              FLIF16TransformContext *t_ctx,
                                              FLIF16RangesContext *ranges);

int ff_flif16_transform_process(FLIF16Context *ctx, FLIF16TransformContext *t_ctx,
                                FLIF16RangesContext *src_ctx, FLIF16PixelData *frame);

void ff_flif16_transform_forward(FLIF16Context *ctx, FLIF16TransformContext *t_ctx,
                                 FLIF16PixelData *pixel_data);

int ff_flif16_transform_write(FLIF16Context *enc_ctx, FLIF16TransformContext *t_ctx,
                              FLIF16RangesContext *src_ctx);

void ff_flif16_transform_reverse(FLIF16Context *ctx, FLIF16TransformContext *t_ctx,
                                 FLIF16PixelData *frame, uint8_t stride_row,
                                 uint8_t stride_col);

void ff_flif16_transforms_close(FLIF16TransformContext *t_ctx);

#endif /* FLIF16_TRANSFORM_H */
