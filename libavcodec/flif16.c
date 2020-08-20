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
void ff_flif16_maniac_ni_prop_ranges_init(FLIF16MinMax *prop_ranges,
                                          unsigned int *prop_ranges_size,
                                          FLIF16RangesContext *ranges,
                                          uint8_t plane,
                                          uint8_t channels)
{
    int min = ff_flif16_ranges_min(ranges, plane);
    int max = ff_flif16_ranges_max(ranges, plane);
    int mind = min - max, maxd = max - min;
    unsigned int top = 0;
    unsigned int size = (((plane < 3) ? plane : 0) + 2 + 5) + ((plane < 3) && (ranges->num_planes > 3));
    *prop_ranges_size = size;
    if (plane < 3) {
        for (int i = 0; i < plane; i++) {
            prop_ranges[top].min   = ff_flif16_ranges_min(ranges, i);
            prop_ranges[top++].max = ff_flif16_ranges_max(ranges, i); // pixels on previous planes
        }
        if (ranges->num_planes > 3)  {
            prop_ranges[top].min   = ff_flif16_ranges_min(ranges, 3);
            prop_ranges[top++].max = ff_flif16_ranges_max(ranges, 3); // pixel on alpha plane
        }
    }
    prop_ranges[top].min = min;
    prop_ranges[top++].max = max; // guess (median of 3)
    prop_ranges[top].min = 0;
    prop_ranges[top++].max = 2; // which predictor was it
    for (int i = 0; i < 5; i++) {
        prop_ranges[top].min = mind;
        prop_ranges[top++].max = maxd;
    }
}

void ff_flif16_maniac_prop_ranges_init(FLIF16MinMax *prop_ranges,
                                       unsigned int *prop_ranges_size,
                                       FLIF16RangesContext *ranges,
                                       uint8_t plane,
                                       uint8_t channels)
{
    int min = ff_flif16_ranges_min(ranges, plane);
    int max = ff_flif16_ranges_max(ranges, plane);
    unsigned int top = 0, pp;
    int mind = min - max, maxd = max - min;
    unsigned int size =   (((plane < 3) ? ((ranges->num_planes > 3) ? plane + 1 : plane) : 0) \
                        + ((plane == 1 || plane == 2) ? 1 : 0) \
                        + ((plane != 2) ? 2 : 0) + 1 + 5);
    *prop_ranges_size = size;

    if (plane < 3) {
        for (pp = 0; pp < plane; pp++) {
            prop_ranges[top].min = ff_flif16_ranges_min(ranges, pp);
            prop_ranges[top++].max = ff_flif16_ranges_max(ranges, pp);
        }
        if (ranges->num_planes > 3) {
            prop_ranges[top].min = ff_flif16_ranges_min(ranges, 3);
            prop_ranges[top++].max = ff_flif16_ranges_max(ranges, 3);
        }
    }

    prop_ranges[top].min = 0;
    prop_ranges[top++].max = 2;

    if (plane == 1 || plane == 2){
        prop_ranges[top].min = ff_flif16_ranges_min(ranges, 0) - ff_flif16_ranges_max(ranges, 0);
        prop_ranges[top++].max = ff_flif16_ranges_max(ranges, 0) - ff_flif16_ranges_min(ranges, 0); // luma prediction miss
    }

    for (int i = 0; i < 4; i++) {
        prop_ranges[top].min = mind;
        prop_ranges[top++].max = maxd;
    }

    prop_ranges[top].min = min;
    prop_ranges[top++].max = max;

    if (plane != 2) {
      prop_ranges[top].min = mind;
      prop_ranges[top++].max = maxd;
      prop_ranges[top].min = mind;
      prop_ranges[top++].max = maxd;
    }
}


int ff_flif16_planes_init(FLIF16Context *s, FLIF16PixelData *frame,
                          int32_t *const_plane_value)
{
    if (frame->seen_before >= 0)
        return 0;

    /* Multiplication overflow is dealt with in the decoder/encoder. */
    for (int i = 0; i < s->num_planes; i++) {
        switch (s->plane_mode[i]) {
        case FLIF16_PLANEMODE_NORMAL:
            frame->data[i] = av_malloc_array(s->width * s->height, sizeof(int32_t));
            if (!frame->data[i])
                return AVERROR(ENOMEM);
            break;

        case FLIF16_PLANEMODE_CONSTANT:
            frame->data[i] = av_malloc(sizeof(int32_t));
            if (!frame->data[i])
                return AVERROR(ENOMEM);
            ((int32_t *) frame->data[i])[0] = const_plane_value[i];
            break;

        case FLIF16_PLANEMODE_FILL:
            frame->data[i] = av_malloc_array(s->width * s->height, sizeof(int32_t));
            if (!frame->data[i])
                return AVERROR(ENOMEM);
            for (int k = 0; k < s->width * s->height; k++)
                    ((int32_t *) frame->data[i])[k] = const_plane_value[i];
            break;

        default:
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}


static void ff_flif16_planes_free(FLIF16PixelData *frame, uint8_t num_planes,
                                uint8_t lookback)
{
    for(uint8_t i = 0; i < (lookback ? MAX_PLANES : num_planes); i++) {
        av_free(frame->data[i]);
    }
}

FLIF16PixelData *ff_flif16_frames_init(uint16_t num_frames)
{
    FLIF16PixelData *frames = av_mallocz_array(num_frames, sizeof(*frames));
    if (!frames)
        return NULL;
    for (int i = 0; i < num_frames; i++)
        frames[i].seen_before = -1;
    return frames;
}

FLIF16PixelData *ff_flif16_frames_resize(FLIF16PixelData *frames,
                                         uint16_t curr_num_frames,
                                         uint16_t new_num_frames)
{
    FLIF16PixelData *new_frames = av_realloc_f(frames, new_num_frames,
                                               sizeof(*frames));
    if (!new_frames)
        return NULL;

    for (int i = curr_num_frames; i < new_num_frames; i++)
        new_frames[i].seen_before = -1;
    return new_frames;
}

void ff_flif16_frames_free(FLIF16PixelData **frames, uint32_t num_frames,
                           uint32_t num_planes, uint8_t lookback)
{
    for (int i = 0; i < num_frames; i++) {
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
