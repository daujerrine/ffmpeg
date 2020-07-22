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
                                                uint8_t plane,
                                                uint8_t channels))[2]
{
    int min = ff_flif16_ranges_min(ranges, plane);
    int max = ff_flif16_ranges_max(ranges, plane);
    int mind = min - max, maxd = max - min;
    int32_t (*prop_ranges)[2];
    unsigned int top = 0;
    unsigned int size = (((plane < 3) ? plane : 0) + 2 + 5) + ((plane < 3) && (ranges->num_planes > 3));
    *prop_ranges_size = size;
    prop_ranges = av_mallocz(sizeof(*prop_ranges) * size);
    if (!prop_ranges)
        return NULL;
    printf("%u size: %u top: %u\n", __LINE__, size, top);
    if (plane < 3) {
        for (int i = 0; i < plane; i++) {
            prop_ranges[top][0]   = ff_flif16_ranges_min(ranges, i);
            prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, i);  // pixels on previous planes
            printf("%u size: %u top: %u\n", __LINE__, size, top);
        }
        if (ranges->num_planes > 3)  {
            prop_ranges[top][0]   = ff_flif16_ranges_min(ranges, 3);
            prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, 3);  // pixel on alpha plane
            printf("%u size: %u top: %u\n", __LINE__, size, top);
        }
    }
    prop_ranges[top][0]   = min;
    prop_ranges[top++][1] = max;  // guess (median of 3)
    printf("%u size: %u top: %u\n", __LINE__, size, top);
    prop_ranges[top][0]   = 0;
    prop_ranges[top++][1] = 2;      // which predictor was it
    printf("%u size: %u top: %u\n", __LINE__, size, top);
    for (int i = 0; i < 5; ++i) {
        prop_ranges[top][0] = mind;
        prop_ranges[top++][1] = maxd;
        printf("%u size: %u top: %u\n", __LINE__, size, top);
    }
    printf("%u size: %u top: %u\n", __LINE__, size, top);
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
    if (!prop_ranges)
        return NULL;
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


static int ff_flif16_plane_alloc(FLIF16PixelData *frame, uint8_t num_planes,
                                 uint32_t width, uint32_t height,
                                 FLIF16PixelType *plane_type,
                                 int32_t *cplane_value) // depth = log2(bpc)
{
    frame->data = av_mallocz(sizeof(*frame->data) * num_planes);
    // TODO if constant, allocate a single integer for the plane.
    // And set is_constant for that plane
    /*
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
        if (num_planes > 3) {
            if(constant_alpha)
                frame->data[3] = av_mallocz(sizeof(int32_t));
            else
                frame->data[3] = av_mallocz(sizeof(int32_t) * width * height);
        }
    }
    if (num_planes> 4)
        frame->data[4] = av_malloc(sizeof(int32_t) * width * height);
    */
    /*
    for (int i = 0; i < num_planes; ++i) {
        switch (pixel_size[i]) {
            case FLIF16_PIXEL_8:
                frame->data[i] = av_mallocz(sizeof(uint8_t) * width * height);
                break;

            case FLIF16_PIXEL_16:
                frame->data[i] = av_mallocz(sizeof(int16_t) * width * height);
                break;

            case FLIF16_PIXEL_16_UNSIGNED:
                frame->data[i] = av_mallocz(sizeof(uint16_t) * width * height);
                break;

            case FLIF16_PIXEL_32:
                frame->data[i] = av_mallocz(sizeof(int32_t) * width * height);
                break;

            // Insignigficant with respect to image size, hence only one type
            // is added here.
            case FLIF16_PIXEL_CONSTANT:
                frame->data[i] = av_mallocz(sizeof(int32_t));
                break;
        }
    }
    */
    for (int i = 0; i < num_planes; ++i) {
        if (plane_type[i] == FLIF16_PIXEL_CONSTANT) {
            frame->data[i] = av_mallocz(sizeof(int32_t));
            ((int32_t *) frame->data[i])[0] = cplane_value[i];
        } else
            frame->data[i] = av_mallocz(sizeof(int32_t) * width * height);
        if (!frame->data[i])
            return -1;
    }

    return 0;
}


static void ff_flif16_plane_free(FLIF16PixelData *frame, uint8_t num_planes)
{
    for(uint8_t i = 0; i < num_planes; ++i) {
        av_free(frame->data[i]);
    }
    av_free(frame->data);
}

FLIF16PixelData *ff_flif16_frames_init(FLIF16Context *s, int32_t *cplane_value)
{
    FLIF16PixelData *frames = av_mallocz(sizeof(*frames) * s->num_frames);
    if (!frames)
        return NULL;

    for (int i = 0; i < s->num_frames; ++i) {
        if (ff_flif16_plane_alloc(&frames[i], s->num_planes, s->width, s->height,
                                  s->plane_type, cplane_value) < 0)
            return NULL;
        frames[i].seen_before = -1;
        //frames[i].palette    = 0;
        // Width?
        // TODO check for the transform first.
        frames[i].col_begin = av_mallocz(s->width * sizeof(*frames->col_begin));
        frames[i].col_end   = av_mallocz(s->width * sizeof(*frames->col_end));
    }
    return frames;
}

void ff_flif16_frames_free(FLIF16PixelData *frames, uint32_t num_frames,
                           uint32_t num_planes)
{
    for(int i = 0; i < num_frames; ++i)
        ff_flif16_plane_free(&frames[i], num_planes);
}
