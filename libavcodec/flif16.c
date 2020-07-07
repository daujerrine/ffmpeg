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

#include "flif16.h"
#include "flif16_transform.h"

/**
 * Initialise property ranges for non interlaced images.
 * @param[out] prop_ranges resultant ranges
 * @param[in]  color ranges of each channel
 * @param[in]  channels number of channels
 */
int32_t  (*ff_flif16_maniac_ni_prop_ranges_init(unsigned int *prop_ranges_size,
                                                FLIF16RangesContext *ranges,
                                                uint8_t property,
                                                uint8_t channels))[2]
{
    int min = ff_flif16_ranges_min(ranges, property);
    int max = ff_flif16_ranges_max(ranges, property);
    int mind = min - max, maxd = max - min;
    int32_t (*prop_ranges)[2];
    unsigned int top = 0;
    unsigned int size = (((property < 3) ? property : 0) + 2 + 5);
    *prop_ranges_size = size;
    prop_ranges = av_mallocz(sizeof(*prop_ranges) * size);
    if (property < 3) {
        for (int i = 0; i < property; i++) {
            prop_ranges[top][0]   = ff_flif16_ranges_min(ranges, i);
            prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, i);  // pixels on previous planes
        }
        if (ranges->num_planes > 3)  {
            prop_ranges[top][0]   = ff_flif16_ranges_min(ranges, 3);
            prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, 3);  // pixel on alpha plane
        }
    }
    prop_ranges[top][0]   = min;
    prop_ranges[top++][1] = max;  // guess (median of 3)
    prop_ranges[top][0]   = 0;
    prop_ranges[top++][1] = 2;      // which predictor was it
    for (int i = 0; i < 5; ++i) {
        prop_ranges[top][0] = mind;
        prop_ranges[top++][1] = maxd;
    }
    return prop_ranges;
}

int32_t (*ff_flif16_maniac_prop_ranges_init(unsigned int *prop_ranges_size,
                                            FLIF16RangesContext *ranges,
                                            uint8_t property,
                                            uint8_t channels))[2]
{
    int min = ff_flif16_ranges_min(ranges, property);
    int max = ff_flif16_ranges_max(ranges, property);
    unsigned int top = 0, pp;
    int mind = min - max, maxd = max - min;
    int32_t (*prop_ranges)[2];
    unsigned int size =   (((property < 3) ? ((ranges->num_planes > 3) ? property + 1 : property) : 0) \
                        + ((property == 1 || property == 2) ? 1 : 0) \
                        + ((property != 2) ? 2 : 0) + 1 + 5);
    prop_ranges = av_mallocz(sizeof(*prop_ranges) * size);
    *prop_ranges_size = size;

    if (property < 3) {
      for (pp = 0; pp < property; pp++) {
        prop_ranges[top][0] = ff_flif16_ranges_min(ranges, pp);
        prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, pp);
      }
      if (ranges->num_planes > 3) {
          prop_ranges[top][0] = ff_flif16_ranges_min(ranges, 3);
          prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, 3);;
      }
    }

    prop_ranges[top][0] = 0;
    prop_ranges[top++][0] = 2;

    if (property == 1 || property == 2){
        prop_ranges[top][0] = ff_flif16_ranges_min(ranges, 0) - ff_flif16_ranges_max(ranges, 0);
        prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, 0) - ff_flif16_ranges_min(ranges, 0); // luma prediction miss
    }
    for (int i = 0; i < 4; ++i) {
        prop_ranges[top][0] = mind;
        prop_ranges[top++][1] = maxd;
    }
    prop_ranges[top][0] = min;
    prop_ranges[top++][0] = max;

    if (property != 2) {
      prop_ranges[top][0] = mind;
      prop_ranges[top++][1] = maxd;
    }
    return prop_ranges;
}


static void ff_flif16_plane_alloc(FLIF16PixelData *frame, uint8_t num_planes,
                                  uint32_t depth, uint32_t width, uint32_t height,
                                  uint8_t constant_alpha) // depth = log2(bpc)
{
    frame->data = av_mallocz(sizeof(*frame->data) * num_planes);
    // TODO if constant, allocate a single integer for the plane.
    // And set is_constant for that plane
    if (depth <= 8) {
        if (num_planes > 0)
            frame->data[0] = av_mallocz(sizeof(int32_t) * width * height);
        if (num_planes > 1)
            frame->data[1] = av_mallocz(sizeof(int32_t) * width * height);
        if (num_planes > 2)
            frame->data[2] = av_mallocz(sizeof(int32_t) * width * height);
        if (num_planes > 3 ) {
            if(constant_alpha)
                frame->data[3] = av_mallocz(sizeof(int32_t) * width * height);
            else
                frame->data[3] = av_mallocz(sizeof(int32_t));
        }
    } else {
        if (num_planes > 0)
            frame->data[0] = av_mallocz(sizeof(int32_t) * width * height);
        if (num_planes > 1)
            frame->data[1] = av_mallocz(sizeof(int32_t) * width * height);
        if (num_planes > 2)
            frame->data[2] = av_mallocz(sizeof(int32_t) * width * height);
        if (num_planes > 3 ) {
            if(constant_alpha)
                frame->data[3] = av_mallocz(sizeof(int32_t) * width * height);
            else
                frame->data[3] = av_mallocz(sizeof(int32_t));
        }
    }
    if (num_planes> 4)
        frame->data[4] = av_malloc(sizeof(int32_t) * width * height);
}


static void ff_flif16_plane_free(FLIF16PixelData *frame, uint8_t num_planes)
{
    for(uint8_t i = 0; i < num_planes; ++i)
        av_free(frame->data[i]);
    av_free(frame->data);
}

FLIF16PixelData *ff_flif16_frames_init(uint32_t num_frames, uint8_t num_planes,
                                       uint32_t depth, uint32_t width, uint32_t height)
{
    FLIF16PixelData *frames = av_mallocz(sizeof(*frames) * num_frames);
    if (!frames)
        return NULL;
    for (int i = 0; i < num_frames; ++i) {
        ff_flif16_plane_alloc(&frames[i], num_planes, depth, width, height, 1);
        frames[i].seen_before = -1;
        frames[i].constant_alpha = 1;
        frames[i].width      = width;
        frames[i].height     = height;
        frames[i].num_planes = num_planes;
        //frames[i].palette    = 0;
    }
    return frames;
}

void ff_flif16_frames_free(FLIF16PixelData *frames, uint32_t num_frames,
                           uint32_t num_planes)
{
    for(int i = 0; i < num_frames; ++i)
        ff_flif16_plane_free(&frames[i], num_planes);
}
