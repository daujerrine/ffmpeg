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
int32_t (*ff_flif16_maniac_ni_prop_ranges_init(unsigned int *prop_ranges_size,
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
    if (plane < 3) {
        for (int i = 0; i < plane; i++) {
            prop_ranges[top][0]   = ff_flif16_ranges_min(ranges, i);
            prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, i);  // pixels on previous planes
        }
        if (ranges->num_planes > 3)  {
            prop_ranges[top][0]   = ff_flif16_ranges_min(ranges, 3);
            prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, 3);  // pixel on alpha plane
        }
    }
    prop_ranges[top][0]   = min;
    prop_ranges[top++][1] = max; // guess (median of 3)
    prop_ranges[top][0]   = 0;
    prop_ranges[top++][1] = 2; // which predictor was it
    for (int i = 0; i < 5; ++i) {
        prop_ranges[top][0] = mind;
        prop_ranges[top++][1] = maxd;
    }
    return prop_ranges;
}

int32_t (*ff_flif16_maniac_prop_ranges_init(unsigned int *prop_ranges_size,
                                            FLIF16RangesContext *ranges,
                                            uint8_t plane,
                                            uint8_t channels))[2]
{
    int min = ff_flif16_ranges_min(ranges, plane);
    int max = ff_flif16_ranges_max(ranges, plane);
    unsigned int top = 0, pp;
    int mind = min - max, maxd = max - min;
    int32_t (*prop_ranges)[2];
    unsigned int size =   (((plane < 3) ? ((ranges->num_planes > 3) ? plane + 1 : plane) : 0) \
                        + ((plane == 1 || plane == 2) ? 1 : 0) \
                        + ((plane != 2) ? 2 : 0) + 1 + 5);
    prop_ranges = av_mallocz(sizeof(*prop_ranges) * size);
    if (!prop_ranges)
        return NULL;
    *prop_ranges_size = size;

    if (plane < 3) {
        for (pp = 0; pp < plane; pp++) {
            prop_ranges[top][0] = ff_flif16_ranges_min(ranges, pp);
            prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, pp);
            printf("min max: %d %d\n", ff_flif16_ranges_min(ranges, pp), ff_flif16_ranges_max(ranges, pp));
        }
        if (ranges->num_planes > 3) {
            prop_ranges[top][0] = ff_flif16_ranges_min(ranges, 3);
            prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, 3);
            printf("min max: %d %d\n", ff_flif16_ranges_min(ranges, pp), ff_flif16_ranges_max(ranges, pp));
        }
    }

    prop_ranges[top][0] = 0;
    prop_ranges[top++][1] = 2;

    if (plane == 1 || plane == 2){
        prop_ranges[top][0] = ff_flif16_ranges_min(ranges, 0) - ff_flif16_ranges_max(ranges, 0);
        prop_ranges[top++][1] = ff_flif16_ranges_max(ranges, 0) - ff_flif16_ranges_min(ranges, 0); // luma prediction miss
    }

    for (int i = 0; i < 4; ++i) {
        prop_ranges[top][0] = mind;
        prop_ranges[top++][1] = maxd;
    }

    prop_ranges[top][0] = min;
    prop_ranges[top++][1] = max;

    if (plane != 2) {
      prop_ranges[top][0] = mind;
      prop_ranges[top++][1] = maxd;
      prop_ranges[top][0] = mind;
      prop_ranges[top++][1] = maxd;
    }
    printf("propranges size: %d\n", size);
    for (int i = 0; i < size; ++i)
        printf("?? propRanges[%d]: (%d, %d)\n", i, prop_ranges[i][0], prop_ranges[i][1]);
    return prop_ranges;
}


int ff_flif16_planes_init(FLIF16Context *s, FLIF16PixelData *frames,
                          uint8_t *plane_mode, uint8_t *const_plane_value,
                          uint8_t lookback)
{
    // for(int i = 0; i < 5; i++){
    //     printf("plane_mode[%d] : %d\n", i, plane_mode[i]);
    // }
    
    for (int j = 0; j < s->num_frames; ++j) {
        if (frames[j].seen_before >= 0)
            continue;

        frames[j].data = av_mallocz(sizeof(*frames->data) * s->num_planes);

        if (!frames[j].data) {
            return AVERROR(ENOMEM);
        }
        // printf("s->num_planes : %d\n", s->num_planes);
        for (int i = 0; i < s->num_planes; ++i) {
            switch (plane_mode[i]) {
                case FLIF16_PLANEMODE_NORMAL:
                    frames[j].data[i] = av_malloc(sizeof(int32_t) * s->width * s->height);
                    break;

                case FLIF16_PLANEMODE_CONSTANT:
                    frames[j].data[i] = av_malloc(sizeof(int32_t));
                    ((int32_t *) frames[j].data[i])[0] = const_plane_value[i];
                    break;

                case FLIF16_PLANEMODE_FILL:
                    frames[j].data[i] = av_malloc(sizeof(int32_t) * s->width * s->height);;
                    if (!frames[j].data[i])
                        return AVERROR(ENOMEM);
                    for (int k = 0; k < s->height * s->width; ++k)
                            ((int32_t *) frames[j].data[i])[k] = const_plane_value[i];
                    break;
            }
        }
    }

    return 0;
}


static void ff_flif16_planes_free(FLIF16PixelData *frame, uint8_t num_planes,
                                uint8_t lookback)
{
    for(uint8_t i = 0; i < (lookback ? MAX_PLANES : num_planes); ++i) {
        av_free(frame->data[i]);
    }
    av_free(frame->data);
}

FLIF16PixelData *ff_flif16_frames_init(FLIF16Context *s)
{
    FLIF16PixelData *frames = av_mallocz(sizeof(*frames) * s->num_frames);
    if (!frames)
        return NULL;

    for (int i = 0; i < s->num_frames; ++i)
        frames[i].seen_before = -1;
    return frames;
}

void ff_flif16_frames_free(FLIF16PixelData **frames, uint32_t num_frames,
                           uint32_t num_planes, uint8_t lookback)
{
    for (int i = 0; i < num_frames; ++i) {
        if ((*frames)[i].seen_before >= 0)
            continue;
        ff_flif16_planes_free(&(*frames)[i], num_planes, lookback);
        if ((*frames)[i].col_begin)
            av_freep(&(*frames)[i].col_begin);
        if ((*frames)[i].col_end)
            av_freep(&(*frames)[i].col_end);
    }

    av_freep(frames);
}
